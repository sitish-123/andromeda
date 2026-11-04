#include "shell/activity/activity_window.h"

#include "system/battery_impact.h"

#include "compositors/shell_wayland_connection.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "dbus/systemd/systemd_unit_name.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_selection_style.h"
#include "system/activity_actions.h"
#include "system/activity_monitor.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/graph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("activity");

  // Design units (DESIGN.md "Activity app"); layout multiplies by the ui scale.
  constexpr float kWindowWidth = 1120.0F;
  constexpr float kWindowHeight = 720.0F;
  constexpr float kWindowMinWidth = 760.0F;
  constexpr float kWindowMinHeight = 480.0F;
  constexpr const float& kSidebarWidth = settings::metrics::kSidebarWidth;
  constexpr float kInspectorWidth = 280.0F;
  constexpr const float& kRowHeight = Tokens::sizeRow;
  constexpr const float& kRowIcon = Tokens::sizeSymbolL;
  constexpr const float& kInspectorIcon = Tokens::sizeControlXl;
  constexpr float kColumnWidth = 80.0F;
  constexpr float kEnergyWidth = 56.0F;
  constexpr const float& kChildRowHeight = Tokens::sizeRowCompact;
  constexpr const float& kChevron = Tokens::sizeSymbol;
  constexpr const float& kSidebarGraphHeight = Tokens::sizeControlS;
  constexpr const float& kInspectorGraphHeight = Tokens::spaceP64;
  constexpr float kHistoryGraphWidth = 160.0F;

  // Values scaled to [0, 1] by their own maximum (Graph wants normalized series); 1 = the largest value.
  std::vector<float> normalized(const std::deque<float>& values, float floor = 1.0F) {
    const float top = std::max(floor, values.empty() ? floor : *std::ranges::max_element(values));
    std::vector<float> out;
    out.reserve(values.size());
    for (const float v : values) {
      out.push_back(std::clamp(v / top, 0.0F, 1.0F));
    }
    return out;
  }

  std::unique_ptr<Graph> makeGraph(std::vector<float> values, float width, float height, ColorRole role) {
    auto graph = std::make_unique<Graph>();
    graph->setFillOpacity(0.18F);
    graph->setColor(colorSpecFromRole(role));
    graph->setValues(std::move(values));
    graph->setSize(width, height);
    return graph;
  }

  // Four bars; the warning colour from three up (a "High energy" app).
  std::unique_ptr<Flex> energyMeter(int bars, float scale) {
    auto meter = ui::row(
        {.align = FlexAlign::End,
         .justify = FlexJustify::End,
         .gap = Tokens::spaceP2 * scale,
         .width = kEnergyWidth * scale}
    );
    for (int i = 0; i < 4; ++i) {
      const bool on = i < bars;
      meter->addChild(
          ui::box({
              .width = Tokens::spaceP4 * scale,
              .height = (Tokens::spaceP4 + Tokens::spaceP2 * static_cast<float>(i + 1)) * scale,
              .configure = [on, bars](Box& box) {
                box.setRadius(1.0F);
                box.setFill(
                    colorSpecFromRole(on ? (bars >= 3 ? ColorRole::Warning : ColorRole::Label2) : ColorRole::Fill3)
                );
              },
          })
      );
    }
    return meter;
  }

  std::unique_ptr<Flex>
  chip(std::string text, float scale, ColorRole fill = ColorRole::Fill2, ColorRole label = ColorRole::Label1) {
    return ui::row(
        {.align = FlexAlign::Center,
         .paddingH = Style::spaceSm * scale,
         .fill = colorSpecFromRole(fill),
         .radius = Tokens::spaceP20 * scale / 2.0F,
         .height = Tokens::spaceP20 * scale},
        ui::label(
            {.text = std::move(text), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(label)}
        )
    );
  }

  std::string memoryText(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return mb >= 1024.0 ? std::format("{:.1f} GB", mb / 1024.0) : std::format("{:.0f} MB", mb);
  }

  std::string cpuText(double percent) { return std::format("{:.1f} %", percent); }

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

  std::string uptimeText(std::uint64_t startedUsec) {
    if (startedUsec == 0) {
      return "–";
    }
    const auto now =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto seconds =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, now - static_cast<std::int64_t>(startedUsec))) / 1'000'000;
    if (seconds >= 3600) {
      return std::format("{} h {} min", seconds / 3600, (seconds % 3600) / 60);
    }
    return std::format("{} min", std::max<std::uint64_t>(1, seconds / 60));
  }

  std::unique_ptr<Label> cell(std::string text, float scale, ColorRole role = ColorRole::Label2) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(role),
        .width = kColumnWidth * scale,
        .textAlign = TextAlign::End,
    });
  }

  std::unique_ptr<Flex> factRow(std::string key, std::string value, float scale, Label** valueOut = nullptr) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Tokens::sizeRowCompact * scale},
        ui::label({
            .text = std::move(key),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .flexGrow = 1.0F,
        }),
        ui::label({
            .out = valueOut,
            .text = std::move(value),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
            .textAlign = TextAlign::End,
        })
    );
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

bool ActivityWindow::inTab(const ActivityRow& row, Tab tab) const {
  if (!m_query.empty()) {
    const auto lower = [](std::string text) {
      std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    };
    const std::string needle = lower(m_query);
    if (!lower(identityFor(row).name).contains(needle) && !lower(row.key).contains(needle)) {
      return false;
    }
  }
  switch (tab) {
  case Tab::Apps:
    return row.hasWindow;
  case Tab::Background:
    return !row.hasWindow;
  case Tab::Containers:
    return false;
  case Tab::Battery:
  case Tab::History:
    return true;
  }
  return false;
}

const ActivityRow* ActivityWindow::selectedRow() const {
  if (m_monitor == nullptr || m_selectedKey.empty()) {
    return nullptr;
  }
  const auto& rows = m_monitor->activity().rows();
  const auto it = std::ranges::find(rows, m_selectedKey, &ActivityRow::key);
  return it != rows.end() ? &*it : nullptr;
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
  if (m_background == nullptr || (!force && now - m_rowStatesAt < kInterval)) {
    return;
  }
  m_rowStates = m_background->classifyRows();
  m_rowStatesAt = now;
}

void ActivityWindow::open(const std::string& rowKey) {
  if (rowKey == "containers" || rowKey.starts_with("ctr:")) {
    m_tab = Tab::Containers;
    m_selectedKey = rowKey == "containers" ? "" : rowKey;
    m_inspectorScroll = {};
    m_containerTicks = 0;
  } else if (!rowKey.empty()) {
    m_selectedKey = rowKey;
    m_inspectorScroll = {};
    if (const ActivityRow* row = selectedRow(); row != nullptr) {
      m_tab = row->hasWindow ? Tab::Apps : Tab::Background;
    }
  }
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
  m_inputDispatcher.pointerLeave();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_surface->setSceneRoot(nullptr);
  m_body = nullptr;
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
}

void ActivityWindow::onSample() {
  refreshContainers();
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
      requestRebuild(); // a unit-list change: new rows show now, rates wait for the next sample
      return;
    }
    double io = 0.0;
    for (const auto& row : activity.rows()) {
      io += row.ioReadBytesPerSec + row.ioWriteBytesPerSec;
    }
    m_diskHistory.push_back(static_cast<float>(io / (1024.0 * 1024.0)));
    while (m_diskHistory.size() > 120) {
      m_diskHistory.pop_front();
    }
  }
  refreshAfterSample();
}

void ActivityWindow::requestRebuild() {
  if (m_surface != nullptr) {
    m_rebuildRequested = true;
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
  if (m_sceneRoot == nullptr || (m_rebuildRequested && !m_buttonDown)) {
    m_rebuildRequested = false;
    const auto start = std::chrono::steady_clock::now();
    buildScene(renderer, width, height);
    kLog.debug(
        "rebuild {:.2f} ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
    );
    return;
  }
  if (needsLayout || m_sceneRoot->width() != width || m_sceneRoot->height() != height) {
    m_sceneRoot->setSize(width, height);
    if (m_body != nullptr) {
      m_body->setSize(width, height);
    }
    m_sceneRoot->layout(renderer);
  }
}

void ActivityWindow::buildScene(Renderer& renderer, float width, float height) {
  const float scale = uiScale();
  refreshRowStates(false);
  // Rows rebuild every sample: the search field moves into the new tree whole, so its caret, selection and
  // preedit survive, and it keeps focus.
  const bool searchFocused = m_searchInput != nullptr && m_inputDispatcher.focusedArea() == m_searchInput->inputArea();
  // A non-null root drops hover (which points into the old tree) but keeps focus; nullptr would also blur the
  // search field and drop its preedit text.
  m_inputDispatcher.setSceneRoot(&m_placeholderRoot);
  m_liveText.clear();
  m_liveGraphs.clear();
  if (m_searchInput != nullptr && m_searchInput->parent() != nullptr) {
    m_keptSearch = m_searchInput->parent()->removeChild(m_searchInput);
  }
  m_searchInput = nullptr;
  m_body = nullptr;
  m_sceneRoot = ui::node({});
  m_sceneRoot->setSize(width, height);
  m_sceneRoot->setAnimationManager(&m_animations);

  auto body = ui::row({.align = FlexAlign::Stretch, .width = width, .height = height});
  body->setFill(colorSpecFromRole(ColorRole::SolidRegular));
  body->addChild(buildSidebar(scale));
  body->addChild(ui::separator({}));
  body->addChild(buildList(renderer, scale));
  body->addChild(ui::separator({}));
  body->addChild(buildInspector(renderer, scale));
  body->setSize(width, height);
  body->layout(renderer);
  m_body = static_cast<Flex*>(m_sceneRoot->addChild(std::move(body)));

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

const ActivityRow* ActivityWindow::findRow(const std::string& key) const {
  const auto& rows = m_monitor->activity().rows();
  const auto it = std::ranges::find(rows, key, &ActivityRow::key);
  return it != rows.end() ? &*it : nullptr;
}

bool ActivityWindow::isFrozen(const ActivityRow& row) const {
  const auto& units = m_monitor->userUnits().units();
  for (const auto& name : row.units) {
    if (const auto it = std::ranges::find(units, name, &SystemdUnit::name);
        it != units.end() && !it->controlGroup.empty()) {
      std::ifstream events(m_monitor->activity().cgroupRoot() + it->controlGroup + "/cgroup.events");
      for (std::string line; std::getline(events, line);) {
        if (line == "frozen 1") {
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

ActivityWindow::Limits ActivityWindow::limitsOf(const ActivityRow& row) const {
  const auto& units = m_monitor->userUnits().units();
  for (const auto& name : row.units) {
    const auto it = std::ranges::find(units, name, &SystemdUnit::name);
    if (it == units.end() || it->controlGroup.empty()) {
      continue;
    }
    const std::string dir = m_monitor->activity().cgroupRoot() + it->controlGroup;
    Limits limits;
    std::string quota;
    double period = 0.0;
    if (std::ifstream cpu(dir + "/cpu.max"); cpu >> quota >> period && quota != "max" && period > 0.0) {
      limits.cpu = nearestIndex(kCpuLimitPercent, std::stod(quota) / period / m_monitor->activity().cpuCount() * 100.0);
    }
    if (std::string high; std::ifstream(dir + "/memory.high") >> high && high != "max") {
      limits.memory = nearestIndex(kMemoryLimitBytes, std::stod(high));
    }
    return limits;
  }
  return {};
}

void ActivityWindow::setSelectedLimits(Limits limits) {
  const ActivityRow* row = selectedRow();
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  const std::uint64_t cpu = limits.cpu == 0 ? ActivityActions::kNoLimit
                                            : static_cast<std::uint64_t>(kCpuLimitPercent[limits.cpu]) * 10'000ULL
                                                  * static_cast<std::uint64_t>(m_monitor->activity().cpuCount());
  const std::uint64_t memory = limits.memory == 0 ? ActivityActions::kNoLimit : kMemoryLimitBytes[limits.memory];
  if (limits.cpu != 0 || limits.memory != 0) {
    m_limitedKeys.insert(row->key);
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
      m_actions->setLimits(*it, cpu, memory, [this](const ActivityActionResult& result) {
        m_actionError = result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
        requestRebuild();
      });
    }
  }
}

std::string ActivityWindow::structureKey() const {
  // Everything that decides which nodes exist: row order and membership, captions, chips, meters, counts, groups.
  std::string key = std::format(
      "{}|{}|{}|{}|{}|{}", static_cast<int>(m_tab), m_query, m_selectedKey, m_actionError, uiScale(),
      m_monitor->sampling()
  );
  for (const auto& row : m_monitor->activity().rows()) {
    const double gpu = m_details != nullptr ? m_details->gpuPercent(row.key) : 0.0;
    key += std::format(
        "\n{}|{}|{}|{}|{}|{}", row.key, row.hasWindow, row.units.size(), captionFor(row), needsAttention(row),
        energyBars(row.cpuPercent, gpu)
    );
    if (const auto* state = rowState(row.key); state != nullptr) {
      key += std::format(
          "|{}|{}|{}|{}", static_cast<int>(state->verdict.finding), state->ignored, state->atLogin,
          BackgroundActivityMonitor::reasonText(*state, row.memoryBytes).empty()
      );
    }
    if (m_limitedKeys.contains(row.key)) {
      const Limits limits = limitsOf(row);
      key += std::format("|L{}{}", limits.cpu, limits.memory);
    }
    if (m_details != nullptr && m_details->expanded(row.key)) {
      key += "|+";
      if (const auto* groups = m_details->groups(row.key); groups != nullptr) {
        for (const auto& group : *groups) {
          key += std::format(",{}:{}", group.command, group.count);
        }
      }
    }
  }
  if (const ActivityRow* row = selectedRow(); row != nullptr && m_details != nullptr) {
    const auto* history = m_details->history(row->key);
    key += history != nullptr && history->memoryMb.size() > 1 ? "|graph" : "|nograph";
    key += isFrozen(*row) ? "|frozen" : "";
    const Limits limits = limitsOf(*row);
    key += std::format("|l{}{}", limits.cpu, limits.memory);
  }
  if (m_containers != nullptr) {
    key += std::format("|c{}", m_containers->runningCount());
    if (m_tab == Tab::Containers) {
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
  if (m_sysmon != nullptr) {
    const auto history = m_sysmon->history();
    key += !history.empty() && history.back().gpuUsagePercent.has_value() ? "|gpu" : "|nogpu";
  }
  return key;
}

void ActivityWindow::updateLive() {
  const auto start = std::chrono::steady_clock::now();
  for (const auto& [label, text] : m_liveText) {
    label->setText(text());
  }
  for (const auto& [graph, values] : m_liveGraphs) {
    graph->setValues(values());
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
  refreshRowStates(false);
  if ((m_tab == Tab::Apps || m_tab == Tab::Background || m_tab == Tab::Containers)
      && m_sceneRoot != nullptr
      && !m_rebuildRequested
      && structureKey() == m_structure) {
    updateLive();
  } else {
    requestRebuild();
  }
}

std::unique_ptr<Flex> ActivityWindow::buildSidebar(float scale) {
  auto sidebar = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Tokens::spaceP2 * scale,
      .paddingV = settings::metrics::kSidebarPaddingV * scale,
      .paddingH = settings::metrics::kSidebarPaddingH * scale,
      .width = kSidebarWidth * scale,
  });
  if (m_keptSearch != nullptr && m_searchScale == scale) {
    m_searchInput = static_cast<Input*>(sidebar->addChild(std::move(m_keptSearch)));
  } else {
    m_keptSearch.reset();
    m_searchScale = scale;
    sidebar->addChild(
        ui::input({
            .out = &m_searchInput,
            .value = m_query,
            .placeholder = i18n::tr("activity.search"),
            .fontSize = Style::fontSizeBody * scale,
            .height = settings::metrics::kSearchFieldHeight * scale,
            .onChange = [this](const std::string& text) {
              m_query = text;
              requestRebuild();
            },
        })
    );
  }
  sidebar->addChild(ui::node({.height = Style::spaceSm * scale}));

  std::size_t apps = 0;
  std::size_t background = 0;
  std::size_t attention = 0;
  for (const auto& row : m_monitor->activity().rows()) {
    (row.hasWindow ? apps : background) += 1;
    attention += needsAttention(row) ? 1 : 0;
  }
  const auto tabRow = [this, scale](Tab tab, std::string glyph, std::string text, std::size_t count, std::size_t warn) {
    const bool selected = m_tab == tab;
    return ui::button({
        .text = std::move(text),
        .glyph = std::move(glyph),
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Tab,
        .customPalette = selected ? std::optional{settings::selectedRowPalette(
                                        settings::selectionStyle().rowFill, settings::selectionStyle().rowLabel
                                    )}
                                  : std::nullopt,
        .minHeight = settings::metrics::kSidebarRowHeight * scale,
        .paddingV = 0.0F,
        .paddingH = settings::metrics::kSidebarRowPaddingH * scale,
        .gap = settings::metrics::kSidebarRowGap * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick =
            [this, tab] {
              m_tab = tab;
              m_listScroll = {};
              requestRebuild();
            },
        .configure =
            [scale, count, warn, tab](Button& button) {
              button.setTabStop(false);
              if (Label* label = button.label(); label != nullptr) {
                label->setFlexGrow(1.0F);
              }
              if (warn > 0) {
                button.addChild(
                    ui::row(
                        {.align = FlexAlign::Center,
                         .justify = FlexJustify::Center,
                         .paddingH = Tokens::spaceP4 * scale,
                         .fill = colorSpecFromRole(ColorRole::Warning),
                         .radius = Tokens::radiusR8 * scale,
                         .minWidth = Tokens::sizeSymbol * scale,
                         .height = Tokens::sizeSymbol * scale},
                        ui::label({
                            .text = std::to_string(warn),
                            .fontSize = Style::fontSizeCaption * scale,
                            .fontWeight = FontWeight::Bold,
                            .color = colorSpecFromRole(ColorRole::AccentText),
                        })
                    )
                );
              }
              if (tab == Tab::Battery || tab == Tab::History) {
                return;
              }
              button.addChild(
                  ui::label({
                      .text = std::to_string(count),
                      .fontSize = Style::fontSizeCaption * scale,
                      .color = colorSpecFromRole(ColorRole::Label2),
                  })
              );
            },
    });
  };
  sidebar->addChild(tabRow(Tab::Apps, "apps", i18n::tr("activity.tab-apps"), apps, 0));
  sidebar->addChild(tabRow(Tab::Background, "stack", i18n::tr("activity.tab-background"), background, attention));
  if (m_containers != nullptr && m_containers->available()) {
    sidebar->addChild(tabRow(
        Tab::Containers, "package", i18n::tr("activity.tab-containers"),
        static_cast<std::size_t>(m_containers->runningCount()), 0
    ));
  }
  sidebar->addChild(tabRow(Tab::Battery, "battery", i18n::tr("activity.tab-battery"), 0, 0));
  sidebar->addChild(tabRow(Tab::History, "history", i18n::tr("activity.tab-history"), 0, 0));
  sidebar->addChild(ui::spacer());
  sidebar->addChild(buildSystemGraphs(scale));
  return sidebar;
}

std::unique_ptr<Flex> ActivityWindow::buildSystemGraphs(float scale) {
  auto box = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  box->addChild(
      ui::label({
          .text = i18n::tr("activity.system"),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Label2),
      })
  );
  const float width = (kSidebarWidth - 2.0F * settings::metrics::kSidebarPaddingH) * scale;
  const auto add = [&](std::string name, std::function<std::string()> value, std::function<std::vector<float>()> values,
                       ColorRole role) {
    Label* valueLabel = nullptr;
    box->addChild(
        ui::row(
            {.align = FlexAlign::Center},
            ui::label(
                {.text = std::move(name),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::Label1),
                 .flexGrow = 1.0F}
            ),
            ui::label(
                {.out = &valueLabel,
                 .text = value(),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::Label2)}
            )
        )
    );
    auto* graph = static_cast<Graph*>(box->addChild(makeGraph(values(), width, kSidebarGraphHeight * scale, role)));
    m_liveText.push_back({valueLabel, std::move(value)});
    m_liveGraphs.push_back({graph, std::move(values)});
  };
  const auto stats = [this] { return m_sysmon != nullptr ? m_sysmon->history() : std::vector<SystemStats>{}; };
  const auto last = [stats] {
    const auto all = stats();
    return all.empty() ? SystemStats{} : all.back();
  };
  const auto series = [stats](auto pick) {
    return [stats, pick] {
      std::deque<float> values;
      for (const auto& sample : stats()) {
        values.push_back(static_cast<float>(pick(sample)));
      }
      return normalized(values, 100.0F);
    };
  };
  add(
      i18n::tr("activity.column-cpu"), [last] { return std::format("{:.0f} %", last().cpuUsagePercent); },
      series([](const SystemStats& s) { return s.cpuUsagePercent; }), ColorRole::Accent
  );
  add(
      i18n::tr("activity.column-memory"),
      [last] {
        const SystemStats now = last();
        return std::format(
            "{:.1f} of {:.0f} GB", static_cast<double>(now.ramUsedMb) / 1024.0,
            static_cast<double>(now.ramTotalMb) / 1024.0
        );
      },
      series([](const SystemStats& s) { return s.ramUsagePercent; }), ColorRole::Accent
  );
  if (last().gpuUsagePercent.has_value()) {
    add(
        i18n::tr("activity.column-gpu"),
        [last] { return std::format("{:.0f} %", last().gpuUsagePercent.value_or(0.0)); },
        series([](const SystemStats& s) { return s.gpuUsagePercent.value_or(0.0); }), ColorRole::Accent
    );
  }
  add(
      i18n::tr("activity.disk"),
      [this] { return std::format("{:.1f} MB/s", m_diskHistory.empty() ? 0.0F : m_diskHistory.back()); },
      [this] { return normalized(m_diskHistory, 1.0F); }, ColorRole::Label2
  );
  return box;
}

std::unique_ptr<Button> ActivityWindow::buildRow(Renderer& renderer, const ActivityRow& row, float scale) {
  const RowIdentity id = identityFor(row);
  const bool selected = row.key == m_selectedKey;
  const int iconPx = static_cast<int>(std::round(kRowIcon * scale));
  Image* icon = nullptr;
  auto image = ui::image({.out = &icon, .width = kRowIcon * scale, .height = kRowIcon * scale});
  if (const std::string path = iconPath(id.icon, iconPx); !path.empty()) {
    icon->setSourceFile(renderer, path, iconPx);
  }
  const std::string caption = captionFor(row);
  return ui::button({
      .text = id.name,
      .fontSize = Style::fontSizeBody * scale,
      .contentAlign = ButtonContentAlign::Start,
      .variant = ButtonVariant::Tab,
      .customPalette = selected ? std::optional{settings::selectedRowPalette(
                                      settings::selectionStyle().rowFill, settings::selectionStyle().rowLabel
                                  )}
                                : std::nullopt,
      .minHeight = kRowHeight * scale,
      .paddingV = 0.0F,
      .paddingH = Style::spaceSm * scale,
      .gap = Style::spaceSm * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick =
          [this, key = row.key] {
            m_selectedKey = key;
            m_inspectorScroll = {};
            m_actionError.clear();
            requestRebuild();
          },
      .configure =
          [&](Button& button) {
            button.setTabStop(false);
            button.insertChildAt(0, std::move(image));
            if (m_details != nullptr) {
              const bool open = m_details->expanded(row.key);
              button.insertChildAt(
                  0,
                  ui::button({
                      .glyph = open ? "chevron-down" : "chevron-right",
                      .glyphSize = Tokens::sizeSymbolInline * scale,
                      .variant = ButtonVariant::Ghost,
                      .padding = 0.0F,
                      .width = kChevron * scale,
                      .height = kChevron * scale,
                      .onClick = [this, key = row.key, open] {
                        m_details->setExpanded(key, !open); // groups fill in on the next sample
                        requestRebuild();
                      },
                  })
              );
            }
            if (Label* name = button.label(); name != nullptr) {
              name->setMaxLines(1);
            }
            button.addChild(
                ui::label({
                    .text = caption,
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::Label3),
                    .maxLines = 1,
                    .ellipsize = TextEllipsize::End,
                    .flexGrow = 1.0F,
                })
            );
            const double gpu = m_details != nullptr ? m_details->gpuPercent(row.key) : 0.0;
            const auto bind = [&](std::unique_ptr<Label> label, auto text) {
              m_liveText.push_back(
                  {static_cast<Label*>(button.addChild(std::move(label))), [this, key = row.key, text] {
                     const ActivityRow* live = findRow(key);
                     return live != nullptr ? text(*live) : std::string();
                   }}
              );
            };
            const Limits limits = m_limitedKeys.contains(row.key) ? limitsOf(row) : Limits{};
            const std::string mark = limits.cpu != 0 || limits.memory != 0 ? "↓ " : "";
            bind(cell(mark + cpuText(row.cpuPercent), scale), [mark](const ActivityRow& r) {
              return mark + cpuText(r.cpuPercent);
            });
            bind(cell(memoryText(row.memoryBytes), scale, ColorRole::Label1), [](const ActivityRow& r) {
              return memoryText(r.memoryBytes);
            });
            bind(cell(std::format("{:.0f} %", gpu), scale), [this](const ActivityRow& r) {
              return std::format("{:.0f} %", m_details != nullptr ? m_details->gpuPercent(r.key) : 0.0);
            });
            button.addChild(energyMeter(energyBars(row.cpuPercent, gpu), scale));
          },
  });
}

void ActivityWindow::addRow(Renderer& renderer, Flex& card, const ActivityRow& row, float scale) {
  card.addChild(buildRow(renderer, row, scale));
  const auto* groups = m_details != nullptr && m_details->expanded(row.key) ? m_details->groups(row.key) : nullptr;
  if (groups == nullptr) {
    return;
  }
  for (const auto& group : *groups) {
    auto cpuCell = cell(cpuText(group.cpuPercent), scale);
    auto memoryCell = cell(memoryText(group.residentBytes), scale);
    const auto bind = [this, key = row.key, command = group.command](Label* label, auto text) {
      m_liveText.push_back({label, [this, key, command, text] {
                              const auto* live = m_details != nullptr ? m_details->groups(key) : nullptr;
                              if (live == nullptr) {
                                return std::string();
                              }
                              const auto it = std::ranges::find(*live, command, &ProcessGroupUsage::command);
                              return it != live->end() ? text(*it) : std::string();
                            }});
    };
    bind(cpuCell.get(), [](const ProcessGroupUsage& g) { return cpuText(g.cpuPercent); });
    bind(memoryCell.get(), [](const ProcessGroupUsage& g) { return memoryText(g.residentBytes); });
    card.addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * scale,
             .paddingH = Style::spaceSm * scale,
             .height = kChildRowHeight * scale},
            ui::node({.width = (kChevron + kRowIcon + Style::spaceSm) * scale}),
            ui::label({
                .text = group.count > 1 ? std::format("{} ({})", group.command, group.count) : group.command,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::Label2),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
                .flexGrow = 1.0F,
            }),
            std::move(cpuCell), std::move(memoryCell), cell("–", scale, ColorRole::Label3),
            ui::node({.width = kEnergyWidth * scale})
        )
    );
  }
}

std::unique_ptr<Flex> ActivityWindow::buildList(Renderer& renderer, float scale) {
  auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});

  auto header = ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceSm * scale,
       .paddingH = (Style::spaceLg + Style::spaceSm + Style::spaceXs) * scale,
       .height = settings::metrics::kTitleBarHeight * scale},
      ui::label({
          .text = i18n::tr("activity.column-name"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .flexGrow = 1.0F,
      })
  );
  const auto headerCell = [&](std::string key, float width) {
    header->addChild(
        ui::label({
            .text = i18n::tr(key),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .width = width * scale,
            .textAlign = TextAlign::End,
        })
    );
  };
  if (m_tab == Tab::Containers) {
    headerCell("activity.column-cpu", kColumnWidth);
    headerCell("activity.column-memory", kColumnWidth);
    headerCell("activity.column-state", kColumnWidth);
  } else if (m_tab == Tab::Battery) {
    headerCell("activity.column-impact", kColumnWidth);
  } else if (m_tab == Tab::History) {
    headerCell("activity.column-24h", kHistoryGraphWidth);
    headerCell("activity.column-peak", kColumnWidth);
  } else {
    headerCell("activity.column-cpu", kColumnWidth);
    headerCell("activity.column-memory", kColumnWidth);
    headerCell("activity.column-gpu", kColumnWidth);
    headerCell("activity.column-energy", kEnergyWidth);
  }
  column->addChild(std::move(header));

  ScrollView* scroll = nullptr;
  column->addChild(
      ui::scrollView({
          .out = &scroll,
          .state = &m_listScroll,
          .scrollbarAutoHide = true,
          .viewportPaddingH = Style::spaceLg * scale,
          .viewportPaddingV = Style::spaceSm * scale,
          .flexGrow = 1.0F,
      })
  );
  Flex& list = *scroll->content();
  list.setAlign(FlexAlign::Stretch);
  list.setGap(Style::spaceLg * scale);

  const auto& rows = m_monitor->activity().rows();
  const bool any = std::ranges::any_of(rows, [this](const ActivityRow& row) { return inTab(row, m_tab); });
  if (!any && m_tab != Tab::Containers) {
    list.addChild(
        ui::label({
            .text = i18n::tr(m_monitor->sampling() && !rows.empty() ? "activity.empty" : "activity.loading"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .textAlign = TextAlign::Center,
        })
    );
    return column;
  }

  const auto addCard = [&](std::string_view caption, auto&& keep) {
    Flex* card = nullptr;
    for (const auto& row : rows) {
      if (!inTab(row, m_tab) || !keep(row)) {
        continue;
      }
      if (card == nullptr) {
        card = settings::addSettingsGroupCard(list, caption, scale);
      }
      addRow(renderer, *card, row, scale);
    }
  };
  const auto finding = [this](const ActivityRow& row, BackgroundVerdict::Finding kind) {
    const auto* state = rowState(row.key);
    return state != nullptr && !state->ignored && state->verdict.finding == kind;
  };
  const auto ignored = [this](const ActivityRow& row) {
    const auto* state = rowState(row.key);
    return state != nullptr && state->ignored;
  };
  if (m_tab == Tab::Apps) {
    Flex* attention = nullptr;
    for (const auto& row : rows) {
      if (!needsAttention(row)) {
        continue;
      }
      if (attention == nullptr) {
        attention = settings::addSettingsGroupCard(list, i18n::tr("activity.group-attention"), scale);
      }
      addRow(renderer, *attention, row, scale);
    }
    addCard(i18n::tr("activity.group-apps"), [](const ActivityRow& row) { return !row.systemManager; });
    addCard(i18n::tr("activity.group-system"), [](const ActivityRow& row) { return row.systemManager; });
  } else if (m_tab == Tab::Containers) {
    addContainerCards(list, scale);
  } else if (m_tab == Tab::Battery) {
    addBatteryCards(renderer, list, scale);
  } else if (m_tab == Tab::History) {
    addHistoryCards(renderer, list, scale);
  } else {
    using Finding = BackgroundVerdict::Finding;
    addCard(i18n::tr("activity.group-uses-resources"), [&](const ActivityRow& row) {
      return finding(row, Finding::UsesResources);
    });
    addCard(i18n::tr("activity.group-duplicates"), [&](const ActivityRow& row) {
      return finding(row, Finding::Duplicate);
    });
    addCard(i18n::tr("activity.group-other-background"), [&](const ActivityRow& row) {
      return !ignored(row) && !finding(row, Finding::UsesResources) && !finding(row, Finding::Duplicate);
    });
    addCard(i18n::tr("activity.group-ignored"), ignored);
  }
  return column;
}

void ActivityWindow::addBatteryCards(Renderer& /*renderer*/, Flex& list, float scale) {
  const UPowerService* upower = m_upowerProvider ? m_upowerProvider() : nullptr;
  const UPowerState battery = upower != nullptr ? upower->state() : UPowerState{};
  Flex* status = settings::addSettingsGroupCard(list, i18n::tr("activity.battery-now"), scale);
  std::string line;
  if (!battery.isPresent) {
    line = i18n::tr("activity.battery-none");
  } else if (battery.onBattery) {
    line = i18n::tr(
        "activity.battery-discharging", "percent", std::format("{:.0f}", battery.percentage), "watts",
        std::format("{:.1f}", battery.energyRate)
    );
  } else {
    line = i18n::tr("activity.battery-plugged", "percent", std::format("{:.0f}", battery.percentage));
  }
  status->addChild(
      ui::label({
          .text = line,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label1),
          .height = kRowHeight * scale,
      })
  );

  Flex* card = nullptr;
  for (const auto& [row, share] :
       rankBatteryImpact(m_monitor->activity().rows(), m_details, [this](const ActivityRow& r) { return inTab(r, Tab::Battery); })) {
    if (card == nullptr) {
      card = settings::addSettingsGroupCard(list, i18n::tr("activity.battery-since-start"), scale);
    }
    const RowIdentity id = identityFor(*row);
    std::string reason = i18n::tr(
        "activity.battery-reason", "cpu", std::format("{:.0f}", static_cast<double>(row->cpuUsageUsec) / 6e7), "uptime",
        uptimeText(row->startedUsec)
    );
    if (battery.onBattery && battery.energyRate > 0.0) {
      reason += std::format(" · ~{:.1f} W", battery.energyRate * share / 100.0);
    }
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * scale,
             .paddingH = Style::spaceSm * scale,
             .minHeight = Tokens::sizeRowTwoCompact * scale},
            ui::column(
                {.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * scale, .flexGrow = 1.0F},
                ui::label(
                    {.text = id.name,
                     .fontSize = Style::fontSizeBody * scale,
                     .color = colorSpecFromRole(ColorRole::Label1),
                     .maxLines = 1,
                     .ellipsize = TextEllipsize::End}
                ),
                ui::label(
                    {.text = reason,
                     .fontSize = Style::fontSizeCaption * scale,
                     .color = colorSpecFromRole(ColorRole::Label2),
                     .maxLines = 1,
                     .ellipsize = TextEllipsize::End}
                )
            ),
            cell(std::format("{:.0f} %", share), scale, ColorRole::Label1)
        )
    );
  }
}

void ActivityWindow::addHistoryCards(Renderer& /*renderer*/, Flex& list, float scale) {
  Flex* card = nullptr;
  for (const auto& row : m_monitor->activity().rows()) {
    const auto* history = m_details != nullptr ? m_details->history(row.key) : nullptr;
    if (!inTab(row, Tab::History) || history == nullptr || history->longMemoryMb.empty()) {
      continue;
    }
    if (card == nullptr) {
      card = settings::addSettingsGroupCard(list, i18n::tr("activity.history-memory"), scale);
    }
    const float peak = *std::ranges::max_element(history->longMemoryMb);
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * scale,
             .paddingH = Style::spaceSm * scale,
             .height = kRowHeight * scale},
            ui::label(
                {.text = identityFor(row).name,
                 .fontSize = Style::fontSizeBody * scale,
                 .color = colorSpecFromRole(ColorRole::Label1),
                 .maxLines = 1,
                 .ellipsize = TextEllipsize::End,
                 .flexGrow = 1.0F}
            ),
            makeGraph(
                normalized(history->longMemoryMb), kHistoryGraphWidth * scale, Tokens::sizeControlS * scale,
                ColorRole::Accent
            ),
            cell(memoryText(static_cast<std::uint64_t>(peak) * 1024 * 1024), scale, ColorRole::Label1)
        )
    );
  }
  if (card == nullptr) {
    list.addChild(
        ui::label({
            .text = i18n::tr("activity.history-empty"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .maxLines = 3,
            .textAlign = TextAlign::Center,
        })
    );
  }
}

std::unique_ptr<Flex> ActivityWindow::buildInspector(Renderer& renderer, float scale) {
  auto inspector = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .width = kInspectorWidth * scale,
  });
  if (m_tab == Tab::Containers) {
    const ContainerInfo* container = m_containers != nullptr && m_selectedKey.starts_with("ctr:")
        ? m_containers->find(m_selectedKey.substr(4))
        : nullptr;
    if (container != nullptr) {
      addContainerInspector(*inspector, *container, scale);
      return inspector;
    }
  }
  const ActivityRow* row = m_tab == Tab::Containers ? nullptr : selectedRow();
  if (row == nullptr) {
    inspector->addChild(
        ui::label({
            .text = i18n::tr("activity.inspector-empty"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
            .maxLines = 3,
        })
    );
    return inspector;
  }

  ScrollView* detailScroll = nullptr;
  inspector->addChild(ui::scrollView({.out = &detailScroll, .state = &m_inspectorScroll, .scrollbarAutoHide = true, .flexGrow = 1.0F}));
  Flex* details = detailScroll->content();
  details->setAlign(FlexAlign::Stretch);
  details->setGap(Style::spaceSm * scale);
  const RowIdentity id = identityFor(*row);
  Image* icon = nullptr;
  const int inspectorPx = static_cast<int>(std::round(kInspectorIcon * scale));
  details->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::image({.out = &icon, .width = kInspectorIcon * scale, .height = kInspectorIcon * scale}),
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * scale, .flexGrow = 1.0F},
              ui::label({
                  .text = id.name,
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::Label1),
                  .maxLines = 1,
                  .ellipsize = TextEllipsize::End,
              }),
              ui::label({
                  .text = unescapeUnitName(row->units.empty() ? row->key : row->units.front()),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::Label2),
                  .maxLines = 1,
                  .ellipsize = TextEllipsize::Middle,
              })
          )
      )
  );
  if (const std::string path = iconPath(id.icon, inspectorPx); !path.empty()) {
    icon->setSourceFile(renderer, path, inspectorPx);
  }

  const double gpu = m_details != nullptr ? m_details->gpuPercent(row->key) : 0.0;
  auto chips = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
  if (!row->hasWindow) {
    chips->addChild(chip(i18n::tr("activity.chip-no-window"), scale));
  }
  if (row->units.size() > 1) {
    chips->addChild(chip(i18n::tr("activity.caption-units", "count", std::to_string(row->units.size())), scale));
  }
  const bool frozen = isFrozen(*row);
  if (frozen) {
    chips->addChild(chip(i18n::tr("activity.chip-paused"), scale));
  }
  const Limits limits = limitsOf(*row);
  if (limits.cpu != 0 || limits.memory != 0) {
    chips->addChild(chip(i18n::tr("activity.chip-limited"), scale, ColorRole::Fill2, ColorRole::Accent));
  }
  if (energyBars(row->cpuPercent, gpu) >= 3) {
    chips->addChild(chip(i18n::tr("activity.chip-high-energy"), scale, ColorRole::Warning, ColorRole::AccentText));
  }
  details->addChild(std::move(chips));

  if (const auto* history = m_details != nullptr ? m_details->history(row->key) : nullptr;
      history != nullptr && history->memoryMb.size() > 1) {
    const float width = (kInspectorWidth - 2.0F * Style::spaceLg) * scale;
    Label* graphValue = nullptr;
    details->addChild(
        ui::row(
            {.align = FlexAlign::Center},
            ui::label(
                {.text = i18n::tr("activity.graph-memory"),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::Label2),
                 .flexGrow = 1.0F}
            ),
            ui::label(
                {.out = &graphValue,
                 .text = memoryText(row->memoryBytes),
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(ColorRole::Label1)}
            )
        )
    );
    auto* graph = static_cast<Graph*>(details->addChild(
        makeGraph(normalized(history->memoryMb), width, kInspectorGraphHeight * scale, ColorRole::Accent)
    ));
    m_liveText.push_back({graphValue, [this, key = row->key] {
                            const ActivityRow* live = findRow(key);
                            return live != nullptr ? memoryText(live->memoryBytes) : std::string();
                          }});
    m_liveGraphs.push_back({graph, [this, key = row->key] {
                              const auto* live = m_details != nullptr ? m_details->history(key) : nullptr;
                              return live != nullptr ? normalized(live->memoryMb) : std::vector<float>{};
                            }});
  }

  const auto fact = [&](std::string name, std::function<std::string(const ActivityRow&)> text) {
    Label* value = nullptr;
    details->addChild(factRow(std::move(name), text(*row), scale, &value));
    m_liveText.push_back({value, [this, key = row->key, text = std::move(text)] {
                            const ActivityRow* live = findRow(key);
                            return live != nullptr ? text(*live) : std::string();
                          }});
  };
  fact(i18n::tr("activity.column-cpu"), [](const ActivityRow& r) { return cpuText(r.cpuPercent); });
  fact(i18n::tr("activity.column-memory"), [](const ActivityRow& r) { return memoryText(r.memoryBytes); });
  fact(i18n::tr("activity.column-gpu"), [this](const ActivityRow& r) {
    return std::format("{:.0f} %", m_details != nullptr ? m_details->gpuPercent(r.key) : 0.0);
  });
  fact(i18n::tr("activity.fact-disk"), [](const ActivityRow& r) {
    return std::format(
        "{:.1f} / {:.1f} MB/s", r.ioReadBytesPerSec / (1024.0 * 1024.0), r.ioWriteBytesPerSec / (1024.0 * 1024.0)
    );
  });
  fact(i18n::tr("activity.fact-network"), [this](const ActivityRow& r) {
    const auto rate = m_details != nullptr ? m_details->networkBytesPerSec(r.key) : std::nullopt;
    if (!rate.has_value()) {
      return std::string("–");
    }
    return *rate >= 1024.0 * 1024.0 ? std::format("{:.1f} MB/s", *rate / (1024.0 * 1024.0))
                                    : std::format("{:.0f} KB/s", *rate / 1024.0);
  });
  fact(i18n::tr("activity.fact-running-for"), [](const ActivityRow& r) { return uptimeText(r.startedUsec); });
  details->addChild(factRow(i18n::tr("activity.fact-units"), std::to_string(row->units.size()), scale));
  const auto* state = rowState(row->key);
  if (state != nullptr && !row->hasWindow) {
    details->addChild(
        factRow(i18n::tr("activity.fact-at-login"), i18n::tr(state->atLogin ? "activity.yes" : "activity.no"), scale)
    );
  }
  if (const std::string reason =
          state != nullptr ? BackgroundActivityMonitor::reasonText(*state, row->memoryBytes) : "";
      !reason.empty()) {
    Label* reasonLabel = nullptr;
    details->addChild(
        ui::row(
            {.align = FlexAlign::Start,
             .gap = Style::spaceSm * scale,
             .padding = Style::spaceSm * scale,
             .fill = colorSpecFromRole(ColorRole::Warning, 0.14F),
             .radius = Style::scaledRadiusMd(scale)},
            ui::glyph({
                .glyph = "alert-circle",
                .glyphSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::Warning),
            }),
            ui::label({
                .out = &reasonLabel,
                .text = reason,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::Label1),
                .maxLines = 4,
                .flexGrow = 1.0F,
            })
        )
    );
    m_liveText.push_back({reasonLabel, [this, key = row->key] {
                            const ActivityRow* live = findRow(key);
                            const auto* liveState = rowState(key);
                            return live != nullptr && liveState != nullptr
                                ? BackgroundActivityMonitor::reasonText(*liveState, live->memoryBytes)
                                : std::string();
                          }});
  }

  if (state != nullptr && !row->hasWindow && m_actions != nullptr) {
    const auto picker = [&](std::string title, std::vector<std::string> labels, std::size_t selected,
                            std::function<void(std::size_t)> onChange) {
      std::vector<ui::SegmentedOption> options;
      for (auto& label : labels) {
        options.push_back({.label = std::move(label)});
      }
      details->addChild(ui::label(
          {.text = std::move(title), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)}
      ));
      details->addChild(ui::segmented({
          .options = std::move(options),
          .selectedIndex = selected,
          .scale = scale,
          .compact = true,
          .equalSegmentWidths = true,
          .onChange = std::move(onChange),
      }));
    };
    details->addChild(ui::label({
        .text = i18n::tr("activity.limit-title"),
        .fontSize = Style::fontSizeCaption * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::Label3),
    }));
    const std::string off = i18n::tr("activity.limit-off");
    picker(i18n::tr("activity.column-cpu"), {off, "50 %", "25 %", "10 %"}, limits.cpu, [this, limits](std::size_t index) {
      setSelectedLimits({.cpu = index, .memory = limits.memory});
    });
    picker(i18n::tr("activity.column-memory"), {off, "1 GB", "512 MB", "256 MB"}, limits.memory, [this, limits](std::size_t index) {
      setSelectedLimits({.cpu = limits.cpu, .memory = index});
    });
    details->addChild(ui::label({
        .text = i18n::tr("activity.limit-resets"),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
    }));
  }

  if (!m_actionError.empty()) {
    inspector->addChild(
        ui::label({
            .text = m_actionError,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Warning),
            .maxLines = 3,
        })
    );
  }
  inspector->addChild(
      ui::button({
          .text = i18n::tr(row->hasWindow ? "activity.action-quit" : "activity.action-stop"),
          .fontSize = Style::fontSizeBody * scale,
          .enabled = m_actions != nullptr,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeight * scale,
          .radius = Style::controlHeight * scale / 2.0F,
          .onClick = [this] { stopSelected(); },
      })
  );
  inspector->addChild(
      ui::button({
          .text = i18n::tr("activity.action-force-quit"),
          .fontSize = Style::fontSizeCaption * scale,
          .enabled = m_actions != nullptr,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeightSm * scale,
          .radius = Style::controlHeightSm * scale / 2.0F,
          .onClick = [this] { forceQuitSelected(); },
      })
  );
  if (state != nullptr && state->atLogin && m_background != nullptr) {
    inspector->addChild(
        ui::button({
            .text = i18n::tr("notifications.internal.activity-dont-start"),
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Default,
            .minHeight = Style::controlHeight * scale,
            .radius = Style::controlHeight * scale / 2.0F,
            .onClick = [this, key = row->key] {
              m_background->disableAtLogin(key, [this](const ActivityActionResult& result) {
                m_actionError = result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
                refreshRowStates(true);
                requestRebuild();
              });
            },
        })
    );
  }
  if (state != nullptr && !row->hasWindow && m_actions != nullptr) {
    inspector->addChild(
        ui::button({
            .text = i18n::tr(frozen ? "activity.action-resume" : "activity.action-pause"),
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Default,
            .minHeight = Style::controlHeight * scale,
            .radius = Style::controlHeight * scale / 2.0F,
            .onClick = [this, frozen] {
              const ActivityRow* selected = selectedRow();
              if (selected == nullptr) {
                return;
              }
              const auto& known = m_monitor->userUnits().units();
              for (const auto& name : selected->units) {
                if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
                  m_actions->setFrozen(*it, !frozen, [this](const ActivityActionResult& result) {
                    m_actionError = result.ok ? ""
                        : result.accessDenied ? i18n::tr("activity.error-admin")
                                              : result.error;
                    requestRebuild();
                  });
                }
              }
            },
        })
    );
  }
  if (state != nullptr && !row->hasWindow && m_background != nullptr) {
    inspector->addChild(
        ui::button({
            .text = i18n::tr(state->ignored ? "activity.action-unignore" : "activity.action-ignore"),
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeight * scale,
            .radius = Style::controlHeight * scale / 2.0F,
            .onClick = [this, key = row->key, ignored = state->ignored] {
              m_background->setIgnored(key, !ignored);
              refreshRowStates(true);
              requestRebuild();
            },
        })
    );
  }
  return inspector;
}

void ActivityWindow::forceQuitSelected() {
  const ActivityRow* row = selectedRow();
  if (row == nullptr || m_actions == nullptr) {
    return;
  }
  const auto& known = m_monitor->userUnits().units();
  for (const auto& name : row->units) {
    if (const auto it = std::ranges::find(known, name, &SystemdUnit::name); it != known.end()) {
      m_actions->forceQuit(*it, [this](const ActivityActionResult& result) {
        if (!result.ok) {
          m_actionError = result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
          requestRebuild();
        }
      });
    }
  }
}

void ActivityWindow::stopSelected() {
  const ActivityRow* row = selectedRow();
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
      m_actionError = result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
      requestRebuild();
    });
  }
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
      const bool inTitleBar = event.sy < settings::metrics::kTitleBarHeight * uiScale();
      if (event.pressed && event.button == BTN_LEFT && inTitleBar && m_inputDispatcher.hoveredArea() == nullptr) {
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
  const InputArea* focused = m_inputDispatcher.focusedArea();
  const bool typing = focused != nullptr
      && (focused->capturesAllKeys() || (m_searchInput != nullptr && focused == m_searchInput->inputArea()));
  if (typing && event.pressed && event.sym == XKB_KEY_Escape) {
    m_inputDispatcher.setFocus(nullptr);
    invalidate();
    return;
  }
  if (typing
      && !(
          event.pressed && (event.modifiers & KeyMod::Ctrl) != 0 && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_q)
      )) {
    m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    invalidate();
    return;
  }
  if (event.pressed && (event.modifiers & KeyMod::Ctrl) != 0 && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_q)) {
    DeferredCall::callLater([this] { close(); });
    return;
  }
  if (event.pressed
      && (event.modifiers & KeyMod::Ctrl) != 0
      && (event.sym == XKB_KEY_f || event.sym == XKB_KEY_F)
      && m_searchInput != nullptr) {
    m_inputDispatcher.setFocus(m_searchInput->inputArea());
    invalidate();
    return;
  }
  if (event.pressed && event.sym == XKB_KEY_Delete && selectedRow() != nullptr) {
    stopSelected();
    return;
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  invalidate();
}

namespace {

  std::string containerStateText(const std::string& state) {
    static const std::map<std::string, std::string> kKeys{
        {"running", "activity.state-running"}, {"paused", "activity.state-paused"},
        {"exited", "activity.state-exited"},   {"created", "activity.state-created"},
        {"restarting", "activity.state-restarting"},
    };
    const auto it = kKeys.find(state);
    return it != kKeys.end() ? i18n::tr(it->second) : state;
  }

  std::string lowerText(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  bool hasUsage(const ContainerInfo& container) { return container.state == "running" || container.state == "paused"; }

  std::uint64_t cgroupMemory(const std::string& root, const std::string& cgroup) {
    if (cgroup.empty()) {
      return 0;
    }
    std::ifstream in(root + cgroup + "/memory.current");
    std::uint64_t bytes = 0;
    in >> bytes;
    return bytes;
  }

} // namespace

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
  if (m_containers == nullptr || !m_containers->available() || m_surface == nullptr) {
    return;
  }
  const bool describe = m_containerTicks % 5 == 0;
  const bool list = describe || m_tab == Tab::Containers;
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

void ActivityWindow::addContainerCards(Flex& list, float scale) {
  const std::string& root = m_monitor->activity().cgroupRoot();
  const auto& containers = m_containers != nullptr ? m_containers->containers() : std::vector<ContainerInfo>{};
  if (m_containers == nullptr || m_containers->engines().empty()) {
    list.addChild(ui::label({
        .text = i18n::tr("activity.loading"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .textAlign = TextAlign::Center,
    }));
    return;
  }
  for (const auto& engine : m_containers->engines()) {
    const std::vector<SystemdUnit> units = engineUnits(engine);
    const auto service = std::ranges::find_if(units, [](const SystemdUnit& u) { return u.name.ends_with(".service"); });
    const bool active = std::ranges::any_of(units, [](const SystemdUnit& u) { return u.activeState == "active"; });
    const bool atLogin = std::ranges::any_of(units, [](const SystemdUnit& u) { return u.unitFileState == "enabled"; });
    const std::string name = engine.name.empty() ? engine.socket : engine.name;
    std::string title;
    if (!engine.error.empty()) {
      title = name;
    } else if (service != units.end() && service->activeState != "active" && active) {
      title = i18n::tr("activity.engine-on-demand", "name", name);
    } else {
      title = i18n::tr("activity.engine-running", "name", name);
    }
    const auto summaryText = [this, root, socket = engine.socket] {
      const auto& all = m_containers->engines();
      const auto live = std::ranges::find(all, socket, &ContainerEngine::socket);
      if (live == all.end()) {
        return std::string();
      }
      if (live->error == "permission") {
        return i18n::tr("activity.engine-permission");
      }
      if (!live->error.empty()) {
        return i18n::tr("activity.engine-error", "error", live->error);
      }
      if (live->idle) {
        return i18n::tr("activity.engine-idle");
      }
      const auto& list = m_containers->containers();
      const auto total = std::ranges::count(list, socket, &ContainerInfo::engineSocket);
      const auto running = std::ranges::count_if(list, [&](const ContainerInfo& c) {
        return c.engineSocket == socket && c.state == "running";
      });
      std::string text =
          i18n::tr("activity.engine-summary", "running", std::to_string(running), "total", std::to_string(total));
      const auto units = engineUnits(*live);
      const auto service = std::ranges::find_if(units, [](const SystemdUnit& u) { return u.name.ends_with(".service"); });
      if (const std::uint64_t bytes = service != units.end() ? cgroupMemory(root, service->controlGroup) : 0; bytes > 0) {
        text += " · " + memoryText(bytes);
      }
      return text;
    };
    const std::string summary = summaryText();
    Label* summaryLabel = nullptr;
    Flex* card = settings::addSettingsGroupCard(list, i18n::tr("activity.group-engine"), scale);
    auto engineRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingH = Style::spaceSm * scale,
         .minHeight = Tokens::sizeRowTwoCompact * scale},
        ui::glyph({
            .glyph = "package",
            .glyphSize = kRowIcon * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        }),
        ui::column(
            {.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * scale, .flexGrow = 1.0F},
            ui::label(
                {.text = title,
                 .fontSize = Style::fontSizeBody * scale,
                 .color = colorSpecFromRole(ColorRole::Label1),
                 .maxLines = 1,
                 .ellipsize = TextEllipsize::End}
            ),
            ui::label(
                {.out = &summaryLabel,
                 .text = summary,
                 .fontSize = Style::fontSizeCaption * scale,
                 .color = colorSpecFromRole(engine.error.empty() ? ColorRole::Label2 : ColorRole::Warning),
                 .maxLines = 2,
                 .ellipsize = TextEllipsize::End}
            )
        )
    );
    if (active && m_actions != nullptr) {
      engineRow->addChild(ui::button({
          .text = i18n::tr("activity.action-stop-engine"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeightSm * scale,
          .radius = Tokens::radiusR8 * scale,
          .onClick = [this, units] {
            const auto done = [this](const ActivityActionResult& result) {
              m_actionError = result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
              m_containerTicks = 0;
              refreshContainers();
              requestRebuild();
            };
            const auto find = [&](const char* suffix) -> std::optional<SystemdUnit> {
              const auto it = std::ranges::find_if(units, [suffix](const SystemdUnit& u) {
                return u.name.ends_with(suffix) && u.activeState == "active";
              });
              return it != units.end() ? std::optional{*it} : std::nullopt;
            };
            const auto service = find(".service");
            const auto stopService = [this, service, done] {
              if (service.has_value()) {
                m_actions->stop(*service, done);
              } else {
                done(ActivityActionResult{.ok = true});
              }
            };
            // The socket first and alone: while it listens, the next client starts the service again.
            if (const auto socket = find(".socket"); socket.has_value()) {
              m_actions->stop(*socket, [stopService, done](const ActivityActionResult& result) {
                result.ok ? stopService() : done(result);
              });
            } else {
              stopService();
            }
          },
      }));
    }
    if (atLogin && m_actions != nullptr) {
      engineRow->addChild(ui::button({
          .text = i18n::tr("notifications.internal.activity-dont-start"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeightSm * scale,
          .radius = Tokens::radiusR8 * scale,
          .onClick = [this, units] {
            for (const auto& unit : units) {
              if (unit.unitFileState != "enabled") {
                continue;
              }
              m_actions->disableAtLogin(unit, {}, {}, [this](const ActivityActionResult& result) {
                m_actionError = result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
                m_containerTicks = 0;
                refreshContainers();
                requestRebuild();
              });
            }
          },
      }));
    }
    card->addChild(std::move(engineRow));
    m_liveText.push_back({summaryLabel, summaryText});
    if (!m_actionError.empty() && !m_selectedKey.starts_with("ctr:")) {
      card->addChild(ui::label({
          .text = m_actionError,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Warning),
          .maxLines = 3,
      }));
    }

    Flex* rows = nullptr;
    const std::string needle = lowerText(m_query);
    for (const auto& container : containers) {
      if (container.engineSocket != engine.socket
          || (!needle.empty() && !lowerText(container.name).contains(needle) && !lowerText(container.image).contains(needle))) {
        continue;
      }
      if (rows == nullptr) {
        rows = settings::addSettingsGroupCard(list, name, scale);
      }
      const std::string key = "ctr:" + container.id;
      const bool selected = key == m_selectedKey;
      rows->addChild(ui::button({
          .text = container.name,
          .glyph = "package",
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .customPalette = selected ? std::optional{settings::selectedRowPalette(
                                          settings::selectionStyle().rowFill, settings::selectionStyle().rowLabel
                                      )}
                                    : std::nullopt,
          .minHeight = kRowHeight * scale,
          .paddingV = 0.0F,
          .paddingH = Style::spaceSm * scale,
          .gap = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, key] {
                m_selectedKey = key;
                m_inspectorScroll = {};
                m_actionError.clear();
                requestRebuild();
              },
          .configure =
              [&](Button& button) {
                button.setTabStop(false);
                if (Label* label = button.label(); label != nullptr) {
                  label->setMaxLines(1);
                }
                button.addChild(ui::label({
                    .text = container.image,
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::Label3),
                    .maxLines = 1,
                    .ellipsize = TextEllipsize::Middle,
                    .flexGrow = 1.0F,
                }));
                const auto bind = [&](std::unique_ptr<Label> label, auto text) {
                  m_liveText.push_back(
                      {static_cast<Label*>(button.addChild(std::move(label))), [this, id = container.id, text] {
                         const ContainerInfo* live = m_containers->find(id);
                         return live != nullptr ? text(*live) : std::string();
                       }}
                  );
                };
                bind(cell(hasUsage(container) ? cpuText(container.cpuPercent) : "–", scale), [](const ContainerInfo& c) {
                  return hasUsage(c) ? cpuText(c.cpuPercent) : std::string("–");
                });
                bind(
                    cell(hasUsage(container) ? memoryText(container.memoryBytes) : "–", scale, ColorRole::Label1),
                    [](const ContainerInfo& c) { return hasUsage(c) ? memoryText(c.memoryBytes) : std::string("–"); }
                );
                button.addChild(cell(containerStateText(container.state), scale));
              },
      }));
    }
  }
  if (containers.empty()) {
    list.addChild(ui::label({
        .text = i18n::tr("activity.containers-empty"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .textAlign = TextAlign::Center,
    }));
  }
}

void ActivityWindow::addContainerInspector(Flex& inspector, const ContainerInfo& container, float scale) {
  const auto engine = std::ranges::find(m_containers->engines(), container.engineSocket, &ContainerEngine::socket);
  const std::string engineName = engine != m_containers->engines().end() ? engine->name : container.engineSocket;
  inspector.addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "package",
          .glyphSize = kInspectorIcon * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * scale, .flexGrow = 1.0F},
          ui::label({
              .text = container.name,
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          }),
          ui::label({
              .text = container.image + " · " + engineName,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .ellipsize = TextEllipsize::Middle,
          })
      )
  ));
  auto chips = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale});
  chips->addChild(chip(containerStateText(container.state), scale));
  inspector.addChild(std::move(chips));
  const auto fact = [&](std::string name, std::function<std::string(const ContainerInfo&)> text) {
    Label* value = nullptr;
    inspector.addChild(factRow(std::move(name), text(container), scale, &value));
    m_liveText.push_back({value, [this, id = container.id, text = std::move(text)] {
                            const ContainerInfo* live = m_containers->find(id);
                            return live != nullptr ? text(*live) : std::string();
                          }});
  };
  fact(i18n::tr("activity.column-cpu"), [](const ContainerInfo& c) {
    return hasUsage(c) ? cpuText(c.cpuPercent) : std::string("–");
  });
  fact(i18n::tr("activity.column-memory"), [](const ContainerInfo& c) {
    return hasUsage(c) ? memoryText(c.memoryBytes) : std::string("–");
  });
  fact(i18n::tr("activity.fact-status"), [](const ContainerInfo& c) { return c.status; });
  inspector.addChild(ui::spacer());
  if (!m_actionError.empty()) {
    inspector.addChild(ui::label({
        .text = m_actionError,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Warning),
        .maxLines = 3,
    }));
  }
  const auto action = [&](const char* key, ContainerService::Action kind) {
    inspector.addChild(ui::button({
        .text = i18n::tr(key),
        .fontSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .minHeight = Style::controlHeight * scale,
        .radius = Style::controlHeight * scale / 2.0F,
        .onClick = [this, id = container.id, kind] {
          if (const ContainerInfo* live = m_containers->find(id); live != nullptr) {
            runContainerAction(*live, static_cast<int>(kind));
          }
        },
    }));
  };
  if (hasUsage(container) || container.state == "restarting") {
    action("activity.action-stop", ContainerService::Action::Stop);
  }
  if (container.state == "running") {
    action("activity.action-pause", ContainerService::Action::Pause);
  } else if (container.state == "paused") {
    action("activity.action-resume", ContainerService::Action::Unpause);
  }
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
          {{"text", button->label() != nullptr ? button->label()->text() : std::string()},
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
