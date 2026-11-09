#include "shell/activity/activity_window.h"

#include "shell/activity/activity_ui.h"
#include "system/battery_impact.h"

#include "compositors/shell_wayland_connection.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "core/shared_worker.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/accessible.h"
#include "system/activity_actions.h"
#include "system/activity_monitor.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/graph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace activity_ui;

namespace {

  constexpr Logger kLog("activity");
  constexpr std::size_t kHistoryPoints = 120;

  constexpr std::array<int, 4> kCpuLimitPercent{0, 50, 25, 10}; // share of all cores, like the CPU column
  constexpr std::array<std::uint64_t, 4> kMemoryLimitBytes{0, 1ULL << 30, 512ULL << 20, 256ULL << 20};

  template <typename T, std::size_t N> std::size_t nearestIndex(const std::array<T, N>& options, double value) {
    std::size_t best = 1;
    for (std::size_t i = 2; i < N; ++i) {
      if (std::abs(static_cast<double>(options[i]) - value) < std::abs(static_cast<double>(options[best]) - value)) {
        best = i;
      }
    }
    return best;
  }

  std::string lowerText(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  void pushPoint(std::deque<float>& history, float value) {
    history.push_back(value);
    while (history.size() > kHistoryPoints) {
      history.pop_front();
    }
  }

  std::string actionError(const ActivityActionResult& result) {
    return result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
  }

} // namespace

ActivityWindow::ActivityWindow() = default;

ActivityWindow::~ActivityWindow() { destroyWindow(); }

void ActivityWindow::initialize(
    ShellWaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, ActivityMonitor* monitor,
    ActivityActions* actions
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_monitor = monitor;
  m_actions = actions;
}

std::string ActivityWindow::iconPath(const std::string& name, int sizePx) {
  if (!name.empty()) {
    if (const std::string& path = m_icons.resolve(name, sizePx); !path.empty()) {
      return path;
    }
  }
  return m_icons.resolve("application-x-executable", sizePx);
}

float ActivityWindow::uiScale() const {
  return m_config != nullptr ? std::max(0.1F, m_config->config().accessibility.uiScale) : 1.0F;
}

bool ActivityWindow::matchesQuery(const ActivityRow& row) const {
  if (m_query.empty()) {
    return true;
  }
  const std::string needle = lowerText(m_query);
  return lowerText(identity(row).name).contains(needle) || lowerText(row.key).contains(needle);
}

bool ActivityWindow::containersAvailable() const { return m_containers != nullptr && m_containers->available(); }

double ActivityWindow::gpuOf(const ActivityRow& row) const {
  return m_details != nullptr ? m_details->gpuPercent(row.key) : 0.0;
}

const ActivityRow* ActivityWindow::selectedRow() const {
  if (m_monitor == nullptr || m_selectedKey.empty() || m_pane == Pane::Containers) {
    return nullptr;
  }
  return findRow(m_selectedKey);
}

const ActivityRow* ActivityWindow::findRow(const std::string& key) const {
  const auto& rows = m_monitor->activity().rows();
  if (const auto it = m_rowIndex.find(key);
      it != m_rowIndex.end() && it->second < rows.size() && rows[it->second].key == key) {
    return &rows[it->second];
  }
  // The rows changed since the index was built (a sample landed before its callback).
  const auto it = std::ranges::find(rows, key, &ActivityRow::key);
  return it != rows.end() ? &*it : nullptr;
}

void ActivityWindow::indexRows() {
  const auto& rows = m_monitor->activity().rows();
  m_rowIndex.clear();
  m_rowIndex.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    m_rowIndex.emplace(rows[i].key, i);
  }
}

const BackgroundActivityMonitor::RowState* ActivityWindow::rowState(const std::string& key) const {
  const auto it = m_rowStates.find(key);
  return it != m_rowStates.end() ? &it->second : nullptr;
}

bool ActivityWindow::needsAttention(const ActivityRow& row) const {
  const auto* state = rowState(row.key);
  return state != nullptr
      && !state->ignored
      && state->verdict.notify
      && state->verdict.finding != BackgroundVerdict::Finding::None;
}

void ActivityWindow::refreshRowStates(bool force) {
  // classifyRows() reads /proc and asks the bus for tray PIDs: often enough for a list, not every 1 s sample.
  constexpr auto kInterval = std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  if (m_background == nullptr || m_surface == nullptr || (!force && now - m_rowStatesAt < kInterval)) {
    return;
  }
  m_rowStatesAt = now;
  if (m_rowStatesReading) {
    m_rowStatesAgain = true;
    return;
  }
  m_rowStatesReading = true;
  m_background->classifyRowsAsync(m_alive, [this](std::map<std::string, BackgroundActivityMonitor::RowState> states) {
    m_rowStatesReading = false;
    m_rowStates = std::move(states);
    if (std::exchange(m_rowStatesAgain, false)) {
      refreshRowStates(true);
    }
    refreshAfterSample();
  });
}

void ActivityWindow::open(const std::string& rowKey) {
  if (rowKey == "containers" || rowKey.starts_with("ctr:")) {
    m_pane = Pane::Containers;
    m_selectedKey = rowKey == "containers" ? "" : rowKey;
    m_inspectorScroll = {};
    m_containerTicks = 0;
  } else if (!rowKey.empty()) {
    if (m_pane == Pane::Containers) {
      m_pane = Pane::Memory;
    }
    m_selectedKey = rowKey;
    m_inspectorScroll = {};
    m_inspectorHidden = false;
    m_confirm = Confirm::None;
  }
  m_orderAt = {};
  if (m_wayland == nullptr || m_renderContext == nullptr || m_monitor == nullptr || !m_wayland->hasXdgShell()) {
    return;
  }
  if (isOpen()) {
    requestRebuild();
    return;
  }
  if (m_surface != nullptr) {
    destroyWindow(); // closed by the compositor, deferred teardown not run yet: finish it before a new surface
  }

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr && !m_wayland->outputs().empty()) {
    output = m_wayland->outputs().front().output;
  }
  m_output = output;

  m_surface = std::make_unique<ToplevelSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setClosedCallback([this, surface = m_surface.get()] {
    // Deferred out of the surface's own callback; a reopen in between already tore this surface down.
    DeferredCall::callLater([this, surface] {
      if (m_surface.get() == surface) {
        destroyWindow();
      }
    });
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  m_surface->setUpdateCallback([] {});

  const float scale = uiScale();
  const ToplevelSurfaceConfig cfg{
      .width = static_cast<std::uint32_t>(std::round(kWindowWidth * scale)),
      .height = static_cast<std::uint32_t>(std::round(kWindowHeight * scale)),
      .minWidth = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale)),
      .minHeight = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale)),
      .title = i18n::tr("activity.title"),
      .appId = "dev.andromeda.Activity",
  };
  if (!m_surface->initialize(output, cfg)) {
    kLog.warn("failed to create the Activity toplevel");
    m_surface.reset();
    return;
  }
  m_pointerInside = false;
  m_actionError.clear();
  m_rowStatesAt = {};
  m_openedAt = std::chrono::steady_clock::now();
  m_skeletonTimer.start(kSkeletonFor, [this] { requestRebuild(); });
  m_monitor->setChangeCallback([this] { onSample(); });
  m_monitor->retain();
  m_sysmon = m_sysmonProvider ? m_sysmonProvider() : nullptr;
  if (m_sysmon != nullptr) {
    m_sysmon->retainSampling();
    m_sysmon->retainGpuUsage();
  }
}

void ActivityWindow::close() { destroyWindow(); }

void ActivityWindow::destroyWindow() {
  if (m_surface == nullptr) {
    return;
  }
  m_skeletonTimer.stop();
  if (m_monitor != nullptr) {
    m_monitor->setChangeCallback({});
    m_monitor->release();
  }
  if (m_sysmon != nullptr) {
    m_sysmon->releaseGpuUsage();
    m_sysmon->releaseSampling();
    m_sysmon = nullptr;
  }
  if (m_details != nullptr) {
    m_details->resetShort();
  }
  m_diskHistory.clear();
  m_pressureHistory.clear();
  m_powerHistory.clear();
  m_inputDispatcher.pointerLeave();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_surface->setSceneRoot(nullptr);
  m_body = nullptr;
  m_liveText.clear();
  m_liveGraphs.clear();
  m_liveHeat.clear();
  m_sceneRoot.reset();
  m_surface.reset();
  m_output = nullptr;
  m_pointerInside = false;
  m_buttonDown = false;
  m_rebuildRequested = false;
  m_actionError.clear();
  m_listScroll = {};
  m_inspectorScroll = {};
  m_rowStates.clear();
  m_searchInput = nullptr;
  m_keptSearch.reset();
  m_query.clear();
  m_confirm = Confirm::None;
  m_order.clear();
}

void ActivityWindow::onSample() {
  refreshContainers();
  readControlsAsync();
  if (m_monitor != nullptr && m_details != nullptr) {
    const auto& activity = m_monitor->activity();
    const auto start = std::chrono::steady_clock::now();
    const bool sampled = m_details->update(
        activity.rows(), m_monitor->userUnits().units(), activity.cgroupRoot(), activity.cpuCount(), start,
        [this] { refreshAfterSample(); }
    );
    kLog.debug(
        "details {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
    );
    if (!sampled) {
      refreshAfterSample(); // a unit-list change: new rows show now, rates wait for the next sample
      return;
    }
    double io = 0.0;
    double pressure = 0.0;
    for (const auto& row : activity.rows()) {
      io += row.ioReadBytesPerSec + row.ioWriteBytesPerSec;
      pressure = std::max(pressure, row.memoryPressure);
    }
    pushPoint(m_diskHistory, static_cast<float>(io / (1024.0 * 1024.0)));
    pushPoint(m_pressureHistory, static_cast<float>(pressure));
    const UPowerService* upower = m_upowerProvider ? m_upowerProvider() : nullptr;
    pushPoint(m_powerHistory, upower != nullptr ? static_cast<float>(upower->state().energyRate) : 0.0F);
  }
  refreshAfterSample();
}

void ActivityWindow::requestRebuild() {
  if (m_surface != nullptr) {
    // Only the request that starts a rebuild reads the controls: their reply refreshes and asks again, and each read
    // sees a new memory.current, so a read per request loops until a frame clears the flag (never while hidden).
    if (!std::exchange(m_rebuildRequested, true)) {
      readControlsAsync();
    }
    m_surface->requestLayout();
  }
}

void ActivityWindow::invalidate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else if (m_sceneRoot->paintDirty()) {
    m_surface->requestRedraw();
  }
}

void ActivityWindow::syncBlur() {
  if (m_surface == nullptr) {
    return;
  }
  if (!m_glass) {
    m_surface->clearBlurRegion();
    return;
  }
  m_surface->setBlurRegion({InputRect{
      .x = 0,
      .y = 0,
      .width = static_cast<int>(m_surface->width()),
      .height = static_cast<int>(m_surface->height()),
  }});
}

void ActivityWindow::prepareFrame(bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();
  // A rebuild drops the pressed area: hold it until the button is released, so no click or drag is lost.
  // A new width rebuilds too: the table drops or adds columns to fit it; so does a height that shows or hides the strip.
  const float stripMin = kStripMinWindowHeight * uiScale();
  const bool resized = m_sceneRoot != nullptr
      && (m_sceneRoot->width() != width || (m_sceneRoot->height() >= stripMin) != (height >= stripMin));
  if (m_sceneRoot == nullptr || ((m_rebuildRequested || resized) && !m_buttonDown)) {
    m_rebuildRequested = false;
    const auto start = std::chrono::steady_clock::now();
    buildScene(renderer, width, height);
    syncBlur();
    kLog.debug(
        "rebuild {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
    );
    return;
  }
  if (needsLayout || m_sceneRoot->height() != height) {
    m_sceneRoot->setSize(width, height);
    if (m_body != nullptr) {
      m_body->setSize(width, height);
    }
    m_sceneRoot->layout(renderer);
    syncBlur();
  }
}

void ActivityWindow::buildScene(Renderer& renderer, float width, float height) {
  const float scale = uiScale();
  if (m_pane == Pane::Containers && !containersAvailable()) {
    m_pane = Pane::Memory;
    m_selectedKey.clear();
  }
  refreshRowStates(false);
  if (m_order.empty() || m_orderAt == std::chrono::steady_clock::time_point{}) {
    computeOrder();
  }
  // Rows rebuild with each structure change: the search field moves into the new tree whole, so its caret,
  // selection and preedit survive, and it keeps focus.
  const bool searchFocused = m_searchInput != nullptr && m_inputDispatcher.focusedArea() == m_searchInput->inputArea();
  // A non-null root drops hover (which points into the old tree) but keeps focus; nullptr would also blur the
  // search field and drop its preedit text.
  m_inputDispatcher.setSceneRoot(&m_placeholderRoot);
  m_liveText.clear();
  m_liveGraphs.clear();
  m_liveHeat.clear();
  m_liveCalls.clear();
  indexRows();
  if (m_searchInput != nullptr && m_searchInput->parent() != nullptr) {
    m_keptSearch = m_searchInput->parent()->removeChild(m_searchInput);
  }
  m_searchInput = nullptr;
  m_body = nullptr;
  m_sceneRoot = ui::node({});
  m_sceneRoot->setSize(width, height);
  m_sceneRoot->setAnimationManager(&m_animations);

  m_glass = m_config != nullptr && m_config->config().shell.settingsWindowTranslucent && !Style::reduceTransparency();
  ColorSpec fill = colorSpecFromRole(ColorRole::SolidRegular);
  if (m_glass) {
    // The Settings window glass: never thinner than the Tinted recipe.
    const float tinted = glass::material(glass::Kind::Sheet, 0.5F, false, isLightPalette()).tint;
    fill = colorSpecFromRole(panelMaterialRole(), std::max(Style::glassMaterial(glass::Kind::Sheet).tint, tinted));
  }
  auto body = ui::column({.align = FlexAlign::Stretch, .fill = fill, .width = width, .height = height});
  body->addChild(buildToolbar(scale));
  const bool containers = m_pane == Pane::Containers;
  if (!containers) {
    if (auto problemCard = buildProblemCard(scale); problemCard != nullptr) {
      body->addChild(std::move(problemCard));
    }
  }
  auto middle = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  const bool inspector = !m_inspectorHidden && (containers ? m_selectedKey.starts_with("ctr:") : selectedRow() != nullptr);
  const float tableWidth = width - (inspector ? kInspectorWidth * scale : 0.0F);
  middle->addChild(containers ? buildContainers(scale) : buildTable(renderer, tableWidth, scale));
  if (inspector) {
    middle->addChild(ui::separator({}));
    middle->addChild(buildInspector(renderer, scale));
  }
  body->addChild(std::move(middle));
  // A short window gives the strip's height to the table and the inspector.
  if (!containers && height >= kStripMinWindowHeight * scale) {
    body->addChild(ui::separator({}));
    body->addChild(buildStrip(width, scale));
  }
  body->setSize(width, height);
  body->layout(renderer);
  m_body = m_sceneRoot->addChild(std::move(body));

  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_surface->setSceneRoot(m_sceneRoot.get());
  if (searchFocused && m_searchInput != nullptr) {
    m_inputDispatcher.setFocus(m_searchInput->inputArea());
  }
  m_structure = structureKey();
}

std::string ActivityWindow::captionFor(const ActivityRow& row) const {
  if (const auto* state = rowState(row.key); state != nullptr && !row.hasWindow) {
    return std::format(
        "{} · {}", i18n::tr(state->atLogin ? "activity.caption-at-login" : "activity.caption-on-demand"),
        uptimeText(row.startedUsec)
    );
  }
  if (row.units.size() > 1) {
    return i18n::tr("activity.caption-units", "count", std::to_string(row.units.size()));
  }
  return {};
}

RowIdentity ActivityWindow::identity(const ActivityRow& row) const {
  RowIdentity id = identityFor(row);
  if (row.appId.empty() && !row.units.empty()) {
    const auto& units = m_monitor->userUnits().units();
    if (const auto it = std::ranges::find(units, row.units.front(), &SystemdUnit::name);
        it != units.end() && !it->description.empty() && it->description != it->name) {
      id.name = it->description;
    }
  }
  return id;
}

const ActivityWindow::CgroupControls* ActivityWindow::controlsOf(const ActivityRow& row) const {
  const auto& units = m_monitor->userUnits().units();
  for (const auto& name : row.units) {
    if (const auto it = std::ranges::find(units, name, &SystemdUnit::name);
        it != units.end() && !it->controlGroup.empty()) {
      const auto found = m_controls.find(it->controlGroup);
      return found != m_controls.end() ? &found->second : nullptr;
    }
  }
  return nullptr;
}

bool ActivityWindow::isFrozen(const ActivityRow& row) const {
  const CgroupControls* controls = controlsOf(row);
  return controls != nullptr && controls->frozen;
}

ActivityWindow::Limits ActivityWindow::limitsOf(const ActivityRow& row) const {
  const CgroupControls* controls = controlsOf(row);
  if (controls == nullptr) {
    return {};
  }
  Limits limits;
  if (controls->cpuShare > 0.0) {
    limits.cpu = nearestIndex(kCpuLimitPercent, controls->cpuShare / m_monitor->activity().cpuCount() * 100.0);
  }
  if (controls->memoryHigh > 0.0) {
    limits.memory = nearestIndex(kMemoryLimitBytes, controls->memoryHigh);
  }
  return limits;
}

void ActivityWindow::readControlsAsync() {
  if (m_monitor == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_controlsReading) {
    m_controlsAgain = true;
    return;
  }
  const auto& units = m_monitor->userUnits().units();
  std::vector<std::string> groups;
  const auto addRow = [&](const ActivityRow& row) {
    for (const auto& name : row.units) {
      if (const auto it = std::ranges::find(units, name, &SystemdUnit::name);
          it != units.end() && !it->controlGroup.empty()) {
        groups.push_back(it->controlGroup);
        return;
      }
    }
  };
  if (const ActivityRow* row = selectedRow(); row != nullptr) {
    addRow(*row);
  }
  for (const auto& key : m_limitedKeys) {
    if (const ActivityRow* row = findRow(key); row != nullptr) {
      addRow(*row);
    }
  }
  if (m_containers != nullptr) {
    for (const auto& engine : m_containers->engines()) {
      for (const auto& unit : engineUnits(engine)) {
        if (unit.name.ends_with(".service") && !unit.controlGroup.empty()) {
          groups.push_back(unit.controlGroup);
        }
      }
    }
  }
  m_controlsReading = true;
  using Controls = std::unordered_map<std::string, CgroupControls>;
  SharedWorker::instance().post<Controls>(
      m_alive,
      [root = m_monitor->activity().cgroupRoot(), groups = std::move(groups)] {
        Controls out;
        for (const auto& group : groups) {
          const std::string dir = root + group;
          CgroupControls controls;
          std::ifstream events(dir + "/cgroup.events");
          for (std::string line; std::getline(events, line);) {
            controls.frozen = controls.frozen || line == "frozen 1";
          }
          std::string quota;
          double period = 0.0;
          if (std::ifstream cpu(dir + "/cpu.max"); cpu >> quota >> period && quota != "max" && period > 0.0) {
            controls.cpuShare = std::stod(quota) / period;
          }
          if (std::string high; std::ifstream(dir + "/memory.high") >> high && high != "max") {
            controls.memoryHigh = std::stod(high);
          }
          std::ifstream(dir + "/memory.current") >> controls.memoryCurrent;
          out.emplace(group, controls);
        }
        return out;
      },
      [this](Controls controls) {
        m_controlsReading = false;
        const bool changed = controls != m_controls;
        m_controls = std::move(controls);
        if (std::exchange(m_controlsAgain, false)) {
          readControlsAsync();
        }
        if (changed) {
          refreshAfterSample();
        }
      }
  );
}

ActivityWindow::Limits ActivityWindow::shownLimits(const ActivityRow& row) const {
  return m_pendingLimits.has_value() && m_pendingLimits->key == row.key ? m_pendingLimits->picks : limitsOf(row);
}

void ActivityWindow::setSelectedLimit(bool cpuField, std::size_t index) {
  const ActivityRow* row = selectedRow();
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  if (!m_pendingLimits.has_value() || m_pendingLimits->key != row->key) {
    // The field the user does not touch keeps its exact cgroup value, even one that is not a preset.
    const CgroupControls* controls = controlsOf(*row);
    m_pendingLimits = PendingLimits{
        .key = row->key,
        .picks = limitsOf(*row),
        .cpuUsec = controls != nullptr && controls->cpuShare > 0.0
            ? static_cast<std::uint64_t>(std::llround(controls->cpuShare * 1e6))
            : ActivityActions::kNoLimit,
        .memoryBytes = controls != nullptr && controls->memoryHigh > 0.0
            ? static_cast<std::uint64_t>(controls->memoryHigh)
            : ActivityActions::kNoLimit,
    };
  }
  PendingLimits& pending = *m_pendingLimits;
  if (cpuField) {
    pending.picks.cpu = index;
    pending.cpuUsec = index == 0 ? ActivityActions::kNoLimit
                                 : static_cast<std::uint64_t>(kCpuLimitPercent[index]) * 10'000ULL
                                       * static_cast<std::uint64_t>(m_monitor->activity().cpuCount());
  } else {
    pending.picks.memory = index;
    pending.memoryBytes = index == 0 ? ActivityActions::kNoLimit : kMemoryLimitBytes[index];
  }
  const std::uint64_t cpu = pending.cpuUsec;
  const std::uint64_t memory = pending.memoryBytes;
  if (pending.picks.cpu != 0 || pending.picks.memory != 0) {
    m_limitedKeys.insert(row->key);
  } else {
    m_limitedKeys.erase(row->key);
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
      m_actions->setLimits(*it, cpu, memory, [this](const ActivityActionResult& result) {
        m_actionError = actionError(result);
        requestRebuild();
      });
    }
  }
}

void ActivityWindow::setSelectedFrozen(bool frozen) {
  const ActivityRow* row = selectedRow();
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
      m_actions->setFrozen(*it, frozen, [this](const ActivityActionResult& result) {
        m_actionError = actionError(result);
        requestRebuild();
      });
    }
  }
}

std::string ActivityWindow::structureKey() const {
  // Everything that decides which nodes exist: pane, order, captions, status marks, groups, cards, inspector parts.
  const bool skeleton =
      !m_monitor->activity().sampled() && std::chrono::steady_clock::now() - m_openedAt < kSkeletonFor;
  std::string key = std::format(
      "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", static_cast<int>(m_pane), m_query, m_selectedKey, m_actionError, uiScale(),
      m_monitor->activity().sampled(), skeleton, sortColumn(), m_processesTab, m_inspectorHidden,
      static_cast<int>(m_confirm), containersAvailable()
  );
  std::string problemKey;
  key += std::format("|p{}:{}", static_cast<int>(problem(&problemKey)), problemKey);
  for (const auto& rowKey : m_order) {
    const ActivityRow* row = findRow(rowKey);
    if (row == nullptr) {
      key += "\n-";
      continue;
    }
    const auto* state = rowState(row->key);
    key += std::format(
        "\n{}|{}|{}|{}|{}", row->key, row->hasWindow, row->units.size(), state != nullptr && state->atLogin,
        needsAttention(*row)
    );
    if (m_limitedKeys.contains(row->key)) {
      const Limits limits = limitsOf(*row);
      key += std::format("|L{}{}{}", limits.cpu, limits.memory, isFrozen(*row));
    }
    if (m_details != nullptr && m_details->expanded(row->key)) {
      key += "|+";
      if (const auto* groups = m_details->groups(row->key); groups != nullptr) {
        for (const auto& group : *groups) {
          key += std::format(",{}:{}", group.command, group.count);
        }
      }
    }
  }
  // A row that joins or leaves the visible set must rebuild before the next re-sort.
  std::size_t visible = 0;
  for (const auto& row : m_monitor->activity().rows()) {
    visible += matchesQuery(row) ? 1 : 0;
  }
  key += std::format("|v{}", visible);
  if (const ActivityRow* row = selectedRow(); row != nullptr) {
    const auto* history = m_details != nullptr ? m_details->history(row->key) : nullptr;
    key += history != nullptr && history->cpuPercent.size() > 1 ? "|graph" : "|nograph";
    key += history != nullptr && !history->longMemoryMb.empty() ? "|long" : "";
    key += isFrozen(*row) ? "|frozen" : "";
    const Limits limits = limitsOf(*row);
    key += std::format("|l{}{}", limits.cpu, limits.memory);
    if (const auto* state = rowState(row->key); state != nullptr) {
      key += std::format(
          "|s{}{}{}", state->ignored, state->atLogin, BackgroundActivityMonitor::reasonText(*state, row->memoryBytes).empty()
      );
    }
  }
  if (m_containers != nullptr) {
    key += std::format("|c{}", m_containers->runningCount());
    if (m_pane == Pane::Containers) {
      for (const auto& engine : m_containers->engines()) {
        key += std::format("\ne{}|{}|{}", engine.socket, engine.name, engine.error);
        for (const auto& unit : engineUnits(engine)) {
          key += std::format("|{}:{}:{}", unit.name, unit.activeState, unit.unitFileState);
        }
      }
      for (const auto& container : m_containers->containers()) {
        key += std::format("\nk{}|{}", container.id, container.state);
      }
    }
  }
  if (m_pane == Pane::Energy) {
    const UPowerService* upower = m_upowerProvider ? m_upowerProvider() : nullptr;
    key += upower != nullptr && upower->state().isPresent ? "|bat" : "|nobat";
  }
  return key;
}

void ActivityWindow::updateLive() {
  const auto start = std::chrono::steady_clock::now();
  for (const auto& [label, text] : m_liveText) {
    if (std::string value = text(); value != label->text()) {
      label->setText(value);
    }
  }
  for (const auto& call : m_liveCalls) {
    call();
  }
  for (const auto& [graph, values] : m_liveGraphs) {
    graph->setValues(values());
  }
  for (const auto& [cell, share] : m_liveHeat) {
    const float value = share();
    if (cell != nullptr) {
      cell->setFill(colorSpecFromRole(ColorRole::Accent, value * kHeatAlpha));
    }
  }
  invalidate();
  kLog.debug(
      "live {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
  );
}

void ActivityWindow::refreshAfterSample() {
  if (m_surface == nullptr) {
    return;
  }
  indexRows();
  refreshRowStates(false);
  if (m_confirm != Confirm::None && selectedRow() == nullptr) {
    m_confirm = Confirm::None;
    requestRebuild();
  }
  if (m_pane == Pane::Containers && !containersAvailable()) {
    setPane(Pane::Memory);
    return;
  }
  // Rows re-sort every few seconds, not every sample: a reorder rebuilds the table, and rows that jump each
  // second cannot be read or clicked.
  std::size_t visible = 0;
  for (const auto& row : m_monitor->activity().rows()) {
    visible += matchesQuery(row) ? 1 : 0;
  }
  const bool sameRows = visible == m_order.size()
      && std::ranges::all_of(m_order, [this](const std::string& key) { return findRow(key) != nullptr; });
  if (!sameRows || std::chrono::steady_clock::now() - m_orderAt >= kResortEvery) {
    computeOrder();
  }
  if (m_sceneRoot != nullptr && !m_rebuildRequested && structureKey() == m_structure) {
    updateLive();
  } else {
    requestRebuild();
  }
}

void ActivityWindow::select(const std::string& key) {
  if (m_selectedKey != key) {
    m_inspectorScroll = {};
    m_confirm = Confirm::None;
    m_processesTab = false;
    m_pendingLimits.reset();
  }
  m_selectedKey = key;
  m_inspectorHidden = false;
  m_actionError.clear();
  requestRebuild();
}

void ActivityWindow::moveSelection(int delta) {
  if (m_order.empty()) {
    return;
  }
  const auto it = std::ranges::find(m_order, m_selectedKey);
  std::ptrdiff_t index = it == m_order.end() ? (delta > 0 ? -1 : static_cast<std::ptrdiff_t>(m_order.size()))
                                             : std::distance(m_order.begin(), it);
  index = std::clamp<std::ptrdiff_t>(index + delta, 0, static_cast<std::ptrdiff_t>(m_order.size()) - 1);
  select(m_order[static_cast<std::size_t>(index)]);
}

void ActivityWindow::setPane(Pane pane) {
  if (pane == Pane::Containers && !containersAvailable()) {
    return;
  }
  if ((pane == Pane::Containers) != (m_pane == Pane::Containers)) {
    m_selectedKey.clear();
    m_inspectorScroll = {};
    m_containerTicks = 0;
  }
  m_pane = pane;
  m_confirm = Confirm::None;
  m_listScroll = {};
  computeOrder();
  requestRebuild();
  if (pane == Pane::Containers) {
    refreshContainers();
  }
}

void ActivityWindow::forceQuitSelected() {
  const ActivityRow* row = selectedRow();
  m_confirm = Confirm::None;
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
      m_actions->forceQuit(*it, [this](const ActivityActionResult& result) {
        if (!result.ok) {
          m_actionError = actionError(result);
          requestRebuild();
        }
      });
    }
  }
  requestRebuild();
}

void ActivityWindow::stopSelected() {
  const ActivityRow* row = selectedRow();
  m_confirm = Confirm::None;
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    const auto it = std::ranges::find(known, name, &SystemdUnit::name);
    if (it == known.end()) {
      continue;
    }
    m_actions->stop(*it, [this](const ActivityActionResult& result) {
      if (result.ok) {
        return;
      }
      m_actionError = actionError(result);
      requestRebuild();
    });
  }
  requestRebuild();
}

bool ActivityWindow::ownsKeyboardSurface(wl_surface* surface) const noexcept {
  return isOpen() && surface != nullptr && surface == m_surface->wlSurface();
}

bool ActivityWindow::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }
  const bool onThis = event.surface != nullptr && event.surface == m_surface->wlSurface();
  bool consumed = false;
  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onThis) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Leave:
    if (onThis) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
      consumed = true;
    }
    break;
  case PointerEvent::Type::Motion:
    if (onThis || m_pointerInside) {
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Button:
    if (onThis || m_pointerInside) {
      m_buttonDown = event.pressed;
      if (!event.pressed && m_rebuildRequested) {
        m_surface->requestLayout();
      }
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      const bool inToolbar = event.sy < kToolbarHeight * uiScale();
      if (event.pressed && event.button == BTN_LEFT && inToolbar && m_inputDispatcher.hoveredArea() == nullptr) {
        m_surface->beginMove(event.serial);
      } else {
        m_inputDispatcher.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
            event.time, event.touch
        );
      }
      consumed = true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (m_pointerInside) {
      m_inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
      consumed = true;
    }
    break;
  }
  invalidate();
  return consumed;
}

void ActivityWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen()) {
    return;
  }
  const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
  const bool closeKey = event.pressed && ctrl && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_q);
  const InputArea* focused = m_inputDispatcher.focusedArea();
  const bool typing = focused != nullptr
      && (focused->capturesAllKeys() || (m_searchInput != nullptr && focused == m_searchInput->inputArea()));
  if (typing && event.pressed && event.sym == XKB_KEY_Escape) {
    m_inputDispatcher.setFocus(nullptr);
    invalidate();
    return;
  }
  if (typing && !closeKey) {
    m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    invalidate();
    return;
  }
  if (closeKey) {
    DeferredCall::callLater([this] { close(); });
    return;
  }
  if (event.pressed) {
    if (ctrl && (event.sym == XKB_KEY_f || event.sym == XKB_KEY_F) && m_searchInput != nullptr) {
      m_inputDispatcher.setFocus(m_searchInput->inputArea());
      invalidate();
      return;
    }
    if (ctrl && event.sym >= XKB_KEY_1 && event.sym <= XKB_KEY_6) {
      setPane(static_cast<Pane>(event.sym - XKB_KEY_1));
      return;
    }
    if (m_confirm != Confirm::None && (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter)) {
      m_confirm == Confirm::ForceQuit ? forceQuitSelected() : stopSelected();
      return;
    }
    if (event.sym == XKB_KEY_Escape) {
      if (m_confirm != Confirm::None) {
        m_confirm = Confirm::None;
      } else {
        m_selectedKey.clear();
      }
      requestRebuild();
      return;
    }
    if (event.sym == XKB_KEY_Delete && selectedRow() != nullptr) {
      m_confirm = (event.modifiers & KeyMod::Shift) != 0 ? Confirm::ForceQuit : Confirm::Quit;
      m_inspectorHidden = false;
      requestRebuild();
      return;
    }
    if (event.sym == XKB_KEY_space && !m_selectedKey.empty()) {
      m_inspectorHidden = !m_inspectorHidden;
      m_confirm = Confirm::None;
      requestRebuild();
      return;
    }
    if ((event.sym == XKB_KEY_Up || event.sym == XKB_KEY_Down) && m_pane != Pane::Containers) {
      moveSelection(event.sym == XKB_KEY_Up ? -1 : 1);
      return;
    }
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  invalidate();
}

std::vector<SystemdUnit> ActivityWindow::engineUnits(const ContainerEngine& engine) const {
  std::vector<SystemdUnit> units;
  for (const auto& name : engine.units) {
    if (const auto it = m_engineUnits.find(engine.socket + "|" + name); it != m_engineUnits.end()) {
      units.push_back(it->second);
    }
  }
  return units;
}

void ActivityWindow::refreshContainers() {
  if (!containersAvailable() || m_surface == nullptr) {
    return;
  }
  const bool describe = m_containerTicks % 5 == 0;
  const bool list = describe || m_pane == Pane::Containers;
  ++m_containerTicks;
  if (!describe || m_actions == nullptr) {
    if (list && (m_engineUnitsRead || m_actions == nullptr)) {
      listContainers();
    }
    return;
  }
  auto pending = std::make_shared<std::size_t>(0);
  for (const auto& engine : m_containers->engines()) {
    *pending += engine.units.size();
  }
  if (*pending == 0) {
    listContainers();
    return;
  }
  for (const auto& engine : m_containers->engines()) {
    for (const auto& name : engine.units) {
      m_actions->describeUnit(
          name, !engine.rootless,
          [this, pending, key = engine.socket + "|" + name](std::optional<SystemdUnit> unit) {
            if (unit.has_value()) {
              m_engineUnits[key] = std::move(*unit);
            } else {
              m_engineUnits.erase(key);
            }
            if (--*pending == 0) {
              listContainers();
            }
          }
      );
    }
  }
}

void ActivityWindow::listContainers() {
  if (m_containers == nullptr || m_surface == nullptr) {
    return;
  }
  m_engineUnitsRead = true;
  std::set<std::string> idle;
  for (const auto& engine : m_containers->engines()) {
    const auto units = engineUnits(engine);
    const auto active = [&](const char* suffix) {
      return std::ranges::any_of(units, [suffix](const SystemdUnit& u) {
        return u.name.ends_with(suffix) && u.activeState == "active";
      });
    };
    if (active(".socket") && !active(".service")) {
      idle.insert(engine.socket);
    }
  }
  m_containers->refresh(std::move(idle), [this] { refreshAfterSample(); });
}

void ActivityWindow::runContainerAction(const ContainerInfo& container, int action) {
  if (m_containers == nullptr) {
    return;
  }
  m_containers->act(container, static_cast<ContainerService::Action>(action), [this](const std::string& error) {
    m_actionError = error == "permission" ? i18n::tr("activity.engine-permission") : error;
    m_containerTicks = 0; // list again now, and re-read the engine units
    refreshContainers();
    requestRebuild();
  });
}

std::string ActivityWindow::buttonsJson() const {
  nlohmann::json out = nlohmann::json::array();
  const std::function<void(const Node&)> walk = [&](const Node& node) {
    if (!node.visible()) {
      return;
    }
    if (const auto* button = dynamic_cast<const Button*>(&node); button != nullptr && button->enabled()) {
      float left = 0.0F;
      float top = 0.0F;
      float right = 0.0F;
      float bottom = 0.0F;
      Node::transformedBounds(&node, left, top, right, bottom);
      out.push_back(
          {{"text", button->label() != nullptr      ? button->label()->text()
                    : button->accessible() != nullptr ? button->accessible()->name
                                                      : std::string()},
           {"x", (left + right) / 2.0F},
           {"y", (top + bottom) / 2.0F}}
      );
    }
    for (const auto& child : node.children()) {
      walk(*child);
    }
  };
  if (m_sceneRoot != nullptr) {
    walk(*m_sceneRoot);
  }
  return out.dump();
}
