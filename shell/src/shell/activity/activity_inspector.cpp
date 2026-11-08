#include "shell/activity/activity_ui.h"
#include "shell/activity/activity_window.h"

#include "dbus/systemd/systemd_unit_name.h"
#include "render/scene/accessible.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_selection_style.h"
#include "system/activity_actions.h"
#include "system/activity_monitor.h"
#include "system/battery_impact.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"

#include <ctime>
#include <map>

using namespace activity_ui;

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

  std::string clockOf(std::uint64_t startedUsec) {
    const std::time_t at = static_cast<std::time_t>(startedUsec / 1'000'000);
    std::tm local{};
    localtime_r(&at, &local);
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
  }

  std::unique_ptr<Label> valueCell(std::string value, float scale, ColorRole role = ColorRole::Label2) {
    return ui::label({
        .text = std::move(value),
        .fontSize = Style::fontSizeBody * scale,
        .fontFeatures = std::string("tnum"),
        .color = colorSpecFromRole(role),
        .width = kColumnWidth * scale,
        .textAlign = TextAlign::End,
    });
  }

  std::unique_ptr<Button> actionButton(std::string label, float scale, std::function<void()> onClick,
                                       ButtonVariant variant = ButtonVariant::Default) {
    return ui::button({
        .text = std::move(label),
        .fontSize = Style::fontSizeBody * scale,
        .variant = variant,
        .minHeight = Style::controlHeight * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Tokens::radiusR8 * scale,
        .onClick = std::move(onClick),
    });
  }

  std::unique_ptr<Label> caption(std::string value, float scale) {
    return text(std::move(value), Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::SemiBold);
  }

} // namespace

std::unique_ptr<Flex> ActivityWindow::buildInspector(Renderer& renderer, float scale) {
  auto inspector = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .width = kInspectorWidth * scale,
  });
  inspector->setPadding(Style::spaceXs * scale, Style::spaceLg * scale, Style::spaceLg * scale, Style::spaceLg * scale);
  if (m_pane == Pane::Containers) {
    const ContainerInfo* container =
        m_containers != nullptr && m_selectedKey.starts_with("ctr:") ? m_containers->find(m_selectedKey.substr(4)) : nullptr;
    if (container != nullptr) {
      addContainerInspector(*inspector, *container, scale);
    }
    return inspector;
  }
  const ActivityRow* row = selectedRow();
  if (row == nullptr) {
    return inspector;
  }

  const RowIdentity id = identity(*row);
  Image* icon = nullptr;
  const int iconPx = static_cast<int>(std::round(kInspectorIcon * scale));
  std::string subtitle;
  if (row->units.size() > 1) {
    subtitle = i18n::tr("activity.caption-units", "count", std::to_string(row->units.size())) + " · ";
  }
  if (row->startedUsec != 0) {
    subtitle += i18n::tr("activity.since", "time", clockOf(row->startedUsec)) + " · ";
  }
  subtitle += i18n::tr(row->systemManager ? "activity.user-system" : "activity.user-you");
  inspector->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::image({.out = &icon, .width = kInspectorIcon * scale, .height = kInspectorIcon * scale}),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          ui::label({
              .text = id.name,
              .fontSize = Tokens::typeTitle3.size * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 2,
              .ellipsize = TextEllipsize::End,
          }),
          ui::label({
              .text = subtitle,
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .ellipsize = TextEllipsize::Middle,
          })
      )
  ));
  if (const std::string path = iconPath(id.icon, iconPx); !path.empty()) {
    icon->setSourceFile(renderer, path, iconPx);
  }
  inspector->addChild(ui::row(
      {.align = FlexAlign::Center},
      ui::segmented({
          .options = std::vector<ui::SegmentedOption>{{.label = i18n::tr("activity.inspector-overview")},
                                                      {.label = i18n::tr("activity.inspector-processes")}},
          .selectedIndex = m_processesTab ? 1U : 0U,
          .fontSize = Style::fontSizeBody * scale,
          .scale = scale,
          .onChange = [this, key = row->key](std::size_t index) {
            m_processesTab = index == 1;
            if (m_processesTab && m_details != nullptr) {
              m_details->setExpanded(key, true); // groups fill in on the next sample
            }
            m_inspectorScroll = {};
            requestRebuild();
          },
      })
  ));

  ScrollView* scroll = nullptr;
  inspector->addChild(ui::scrollView({.out = &scroll, .state = &m_inspectorScroll, .scrollbarAutoHide = true, .flexGrow = 1.0F}));
  Flex* details = scroll->content();
  details->setAlign(FlexAlign::Stretch);
  details->setGap(Style::spaceMd * scale);
  if (m_processesTab) {
    addProcesses(*details, *row, scale);
  } else {
    addOverview(*details, *row, scale);
  }
  addActions(*inspector, *row, scale);
  return inspector;
}

void ActivityWindow::addOverview(Flex& details, const ActivityRow& row, float scale) {
  const float innerWidth = (kInspectorWidth - 4.0F * Style::spaceLg) * scale;
  const auto live = [this, key = row.key](std::function<std::string(const ActivityRow&)> text) {
    return [this, key, text = std::move(text)] {
      const ActivityRow* found = findRow(key);
      return found != nullptr ? text(*found) : std::string();
    };
  };
  const auto* history = m_details != nullptr ? m_details->history(row.key) : nullptr;

  // CPU over the last minute.
  {
    auto cpuCard = card(m_glass, scale);
    cpuCard->setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
    cpuCard->setGap(Style::spaceSm * scale);
    Label* value = nullptr;
    auto cpuNow = live([](const ActivityRow& r) { return cpuText(r.cpuPercent); });
    cpuCard->addChild(ui::row(
        {.align = FlexAlign::Center},
        caption(i18n::tr("activity.inspector-cpu"), scale), ui::spacer(),
        text(cpuNow(), Style::fontSizeCaption * scale, ColorRole::Label1, FontWeight::SemiBold, &value)
    ));
    m_liveText.push_back({value, std::move(cpuNow)});
    if (history != nullptr && history->cpuPercent.size() > 1) {
      const auto minute = [this, key = row.key] {
        const auto* found = m_details != nullptr ? m_details->history(key) : nullptr;
        if (found == nullptr) {
          return std::vector<float>{};
        }
        const auto& all = found->cpuPercent;
        const auto from = all.size() > 60 ? all.end() - 60 : all.begin();
        return normalized(std::vector<float>(from, all.end()), 10.0F);
      };
      auto* graph = static_cast<Graph*>(
          cpuCard->addChild(makeGraph(minute(), innerWidth, kMiniGraphHeight * scale, ColorRole::Accent))
      );
      m_liveGraphs.push_back({graph, minute});
    }
    details.addChild(std::move(cpuCard));
  }

  // Use now.
  {
    auto facts = card(m_glass, scale);
    const auto fact = [&](std::string name, std::function<std::string(const ActivityRow&)> textOf) {
      Label* value = nullptr;
      facts->addChild(cardRow(std::move(name), textOf(row), scale, &value));
      m_liveText.push_back({value, live(std::move(textOf))});
    };
    fact(i18n::tr("activity.column-memory"), [](const ActivityRow& r) { return memoryText(r.memoryBytes); });
    fact(i18n::tr("activity.column-energy"), [this](const ActivityRow& r) {
      return std::format("{} · GPU {:.0f} %", energyText(energyBars(r.cpuPercent, gpuOf(r))), gpuOf(r));
    });
    fact(i18n::tr("activity.disk"), [](const ActivityRow& r) {
      return std::format("{} / {}", rateText(r.ioReadBytesPerSec), rateText(r.ioWriteBytesPerSec));
    });
    fact(i18n::tr("activity.fact-network"), [this](const ActivityRow& r) {
      const auto rate = m_details != nullptr ? m_details->networkBytesPerSec(r.key) : std::nullopt;
      return rate.has_value() ? rateText(*rate) : std::string("–");
    });
    // The QoS tier the compositor gives the app now (docs/qos.md §3).
    fact(i18n::tr("activity.fact-priority"), [](const ActivityRow& r) {
      const std::string_view tier = qosTierName(r.qosTier);
      return tier.empty() ? std::string("–") : i18n::tr(std::format("activity.tier-{}", tier));
    });
    fact(i18n::tr("activity.fact-running-for"), [](const ActivityRow& r) { return uptimeText(r.startedUsec); });
    const auto* state = rowState(row.key);
    if (state != nullptr && !row.hasWindow) {
      facts->addChild(cardRow(
          i18n::tr("activity.fact-at-login"), i18n::tr(state->atLogin ? "activity.yes" : "activity.no"), scale
      ));
    }
    if (!row.units.empty()) {
      facts->addChild(cardRow(i18n::tr("activity.fact-units"), unescapeUnitName(row.units.front()), scale));
    }
    settings::addSettingsRowSeparators(*facts, scale);
    details.addChild(std::move(facts));
  }

  // Memory over 24 hours, one point per background check.
  if (history != nullptr && !history->longMemoryMb.empty()) {
    auto day = card(m_glass, scale);
    day->setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
    day->setGap(Style::spaceSm * scale);
    const float peak = *std::ranges::max_element(history->longMemoryMb);
    day->addChild(ui::row(
        {.align = FlexAlign::Center},
        caption(i18n::tr("activity.column-24h"), scale), ui::spacer(),
        text(i18n::tr("activity.peak", "value", memoryText(static_cast<std::uint64_t>(peak) * 1024 * 1024)),
             Style::fontSizeCaption * scale, ColorRole::Label1, FontWeight::SemiBold)
    ));
    day->addChild(makeGraph(normalized(history->longMemoryMb), innerWidth, kMiniGraphHeight * scale, ColorRole::Accent));
    details.addChild(std::move(day));
  }

  const auto* state = rowState(row.key);
  if (const std::string reason = state != nullptr ? BackgroundActivityMonitor::reasonText(*state, row.memoryBytes) : "";
      !reason.empty()) {
    Label* reasonLabel = nullptr;
    details.addChild(ui::row(
        {.align = FlexAlign::Start,
         .gap = Style::spaceSm * scale,
         .padding = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::Warning, 0.14F),
         .radius = Tokens::radiusR8 * scale},
        ui::glyph({
            .glyph = "alert-triangle",
            .glyphSize = Tokens::sizeSymbol * scale,
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
    ));
    m_liveText.push_back({reasonLabel, [this, key = row.key] {
                            const ActivityRow* found = findRow(key);
                            const auto* liveState = rowState(key);
                            return found != nullptr && liveState != nullptr
                                ? BackgroundActivityMonitor::reasonText(*liveState, found->memoryBytes)
                                : std::string();
                          }});
  }

  if (state == nullptr || row.hasWindow || m_actions == nullptr) {
    return;
  }
  const Limits limits = shownLimits(row);
  auto limitCard = card(m_glass, scale);
  limitCard->setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
  limitCard->setGap(Style::spaceSm * scale);
  limitCard->addChild(caption(i18n::tr("activity.limit-title"), scale));
  const auto picker = [&](std::string title, std::vector<std::string> labels, std::size_t selected,
                          std::function<void(std::size_t)> onChange) {
    std::vector<ui::SegmentedOption> options;
    for (auto& label : labels) {
      options.push_back({.label = std::move(label)});
    }
    limitCard->addChild(text(std::move(title), Style::fontSizeCaption * scale, ColorRole::Label2));
    limitCard->addChild(ui::segmented({
        .options = std::move(options),
        .selectedIndex = selected,
        .scale = scale,
        .compact = true,
        .equalSegmentWidths = true,
        .onChange = std::move(onChange),
    }));
  };
  const std::string off = i18n::tr("activity.limit-off");
  picker(i18n::tr("activity.column-cpu"), {off, "50 %", "25 %", "10 %"}, limits.cpu, [this](std::size_t index) {
    setSelectedLimit(true, index);
  });
  picker(i18n::tr("activity.column-memory"), {off, "1 GB", "512 MB", "256 MB"}, limits.memory, [this](std::size_t index) {
    setSelectedLimit(false, index);
  });
  limitCard->addChild(text(i18n::tr("activity.limit-resets"), Style::fontSizeCaption * scale, ColorRole::Label3));
  details.addChild(std::move(limitCard));

  auto buttons = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
  const bool frozen = isFrozen(row);
  buttons->addChild(actionButton(i18n::tr(frozen ? "activity.action-resume" : "activity.action-pause"), scale,
                                 [this, frozen] { setSelectedFrozen(!frozen); }));
  if (m_background != nullptr) {
    buttons->addChild(actionButton(
        i18n::tr(state->ignored ? "activity.action-unignore" : "activity.action-ignore"), scale,
        [this, key = row.key, ignored = state->ignored] {
          m_background->setIgnored(key, !ignored);
          m_rowStates[key].ignored = !ignored; // shows now; the full re-classify lands from the worker
          refreshRowStates(true);
          requestRebuild();
        }
    ));
    if (state->atLogin) {
      buttons->addChild(actionButton(i18n::tr("notifications.internal.activity-dont-start"), scale, [this, key = row.key] {
        m_background->disableAtLogin(key, [this](const ActivityActionResult& result) {
          m_actionError = result.ok ? "" : result.accessDenied ? i18n::tr("activity.error-admin") : result.error;
          refreshRowStates(true);
          requestRebuild();
        });
      }));
    }
  }
  details.addChild(std::move(buttons));
}

void ActivityWindow::addProcesses(Flex& details, const ActivityRow& row, float scale) {
  const auto* groups = m_details != nullptr ? m_details->groups(row.key) : nullptr;
  if (groups == nullptr || groups->empty()) {
    details.addChild(ui::label({
        .text = i18n::tr("activity.processes-empty"),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 3,
    }));
    return;
  }
  auto list = card(m_glass, scale);
  for (const auto& group : *groups) {
    Label* cpu = nullptr;
    Label* memory = nullptr;
    auto cpuCell = valueCell(cpuText(group.cpuPercent), scale);
    auto memoryCell = valueCell(memoryText(group.residentBytes), scale, ColorRole::Label1);
    cpu = cpuCell.get();
    memory = memoryCell.get();
    list->addChild(ui::row(
        {.align = FlexAlign::Center, .minHeight = Tokens::sizeRow * scale},
        ui::label({
            .text = group.count > 1 ? std::format("{} ({})", group.command, group.count) : group.command,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 1,
            .ellipsize = TextEllipsize::Middle,
            .flexGrow = 1.0F,
        }),
        std::move(cpuCell), std::move(memoryCell)
    ));
    const auto bind = [this, key = row.key, command = group.command](Label* label, auto textOf) {
      m_liveText.push_back({label, [this, key, command, textOf] {
                              const auto* live = m_details != nullptr ? m_details->groups(key) : nullptr;
                              if (live == nullptr) {
                                return std::string();
                              }
                              const auto it = std::ranges::find(*live, command, &ProcessGroupUsage::command);
                              return it != live->end() ? textOf(*it) : std::string();
                            }});
    };
    bind(cpu, [](const ProcessGroupUsage& g) { return cpuText(g.cpuPercent); });
    bind(memory, [](const ProcessGroupUsage& g) { return memoryText(g.residentBytes); });
  }
  settings::addSettingsRowSeparators(*list, scale);
  details.addChild(std::move(list));
}

void ActivityWindow::addActions(Flex& inspector, const ActivityRow& row, float scale) {
  if (!m_actionError.empty()) {
    inspector.addChild(ui::label({
        .text = m_actionError,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Warning),
        .maxLines = 3,
    }));
  }
  const std::string name = identity(row).name;
  if (m_confirm != Confirm::None) {
    const bool force = m_confirm == Confirm::ForceQuit;
    inspector.addChild(ui::label({
        .text = force ? i18n::tr("activity.confirm-force", "name", name)
                      : i18n::tr(row.hasWindow ? "activity.confirm-quit" : "activity.confirm-stop", "name", name),
        .fontSize = Tokens::typeHeadline.size * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 3,
    }));
    auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale});
    buttons->addChild(actionButton(i18n::tr("activity.action-cancel"), scale, [this] {
      m_confirm = Confirm::None;
      requestRebuild();
    }));
    buttons->addChild(actionButton(
        i18n::tr(force ? "activity.action-force-quit" : row.hasWindow ? "activity.action-quit" : "activity.action-stop"),
        scale, [this, force] { force ? forceQuitSelected() : stopSelected(); },
        force ? ButtonVariant::Destructive : ButtonVariant::Primary
    ));
    inspector.addChild(std::move(buttons));
    return;
  }
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale});
  buttons->addChild(actionButton(i18n::tr("activity.action-force-quit-ask"), scale, [this] {
    m_confirm = Confirm::ForceQuit;
    requestRebuild();
  }));
  buttons->addChild(actionButton(i18n::tr(row.hasWindow ? "activity.action-quit-ask" : "activity.action-stop-ask"), scale, [this] {
    m_confirm = Confirm::Quit;
    requestRebuild();
  }));
  inspector.addChild(std::move(buttons));
}

std::unique_ptr<Flex> ActivityWindow::buildContainers(float scale) {
  auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  ScrollView* scroll = nullptr;
  column->addChild(ui::scrollView({
      .out = &scroll,
      .state = &m_listScroll,
      .scrollbarAutoHide = true,
      .viewportPaddingH = Style::spaceLg * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  }));
  Flex& list = *scroll->content();
  list.setAlign(FlexAlign::Stretch);
  list.setGap(Style::spaceLg * scale);
  addContainerCards(list, scale);
  return column;
}

void ActivityWindow::addContainerCards(Flex& list, float scale) {
  const auto& containers = m_containers != nullptr ? m_containers->containers() : std::vector<ContainerInfo>{};
  if (m_containers == nullptr || m_containers->engines().empty()) {
    list.addChild(text(i18n::tr("activity.loading"), Style::fontSizeBody * scale, ColorRole::Label2));
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
    const auto summaryText = [this, socket = engine.socket] {
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
      const auto controls = service != units.end() ? m_controls.find(service->controlGroup) : m_controls.end();
      if (const std::uint64_t bytes = controls != m_controls.end() ? controls->second.memoryCurrent : 0; bytes > 0) {
        text += " · " + memoryText(bytes);
      }
      return text;
    };
    Label* summaryLabel = nullptr;
    Flex* engineCard = settings::addSettingsGroupCard(list, i18n::tr("activity.group-engine"), scale);
    auto engineRow = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Tokens::sizeRowTwoCompact * scale},
        ui::glyph({
            .glyph = "package",
            .glyphSize = Tokens::sizeSymbolL * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        }),
        ui::column(
            {.align = FlexAlign::Stretch, .gap = Tokens::spaceP2 * scale, .flexGrow = 1.0F},
            text(title, Style::fontSizeBody * scale, ColorRole::Label1),
            ui::label({
                .out = &summaryLabel,
                .text = summaryText(),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(engine.error.empty() ? ColorRole::Label2 : ColorRole::Warning),
                .maxLines = 2,
                .ellipsize = TextEllipsize::End,
            })
        )
    );
    if (active && m_actions != nullptr) {
      engineRow->addChild(actionButton(i18n::tr("activity.action-stop-engine"), scale, [this, units] {
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
        const auto stopService = [this, service = find(".service"), done] {
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
      }));
    }
    if (atLogin && m_actions != nullptr) {
      engineRow->addChild(actionButton(i18n::tr("notifications.internal.activity-dont-start"), scale, [this, units] {
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
      }));
    }
    engineCard->addChild(std::move(engineRow));
    m_liveText.push_back({summaryLabel, summaryText});
    if (!m_actionError.empty() && !m_selectedKey.starts_with("ctr:")) {
      engineCard->addChild(ui::label({
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
          .radius = Tokens::radiusR4 * scale,
          .onClick =
              [this, key] {
                m_selectedKey = key;
                m_inspectorScroll = {};
                m_inspectorHidden = false;
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
                const auto bind = [&](std::unique_ptr<Label> label, auto textOf) {
                  m_liveText.push_back(
                      {static_cast<Label*>(button.addChild(std::move(label))), [this, id = container.id, textOf] {
                         const ContainerInfo* live = m_containers->find(id);
                         return live != nullptr ? textOf(*live) : std::string();
                       }}
                  );
                };
                bind(valueCell(hasUsage(container) ? cpuText(container.cpuPercent) : "–", scale),
                     [](const ContainerInfo& c) { return hasUsage(c) ? cpuText(c.cpuPercent) : std::string("–"); });
                bind(
                    valueCell(hasUsage(container) ? memoryText(container.memoryBytes) : "–", scale, ColorRole::Label1),
                    [](const ContainerInfo& c) { return hasUsage(c) ? memoryText(c.memoryBytes) : std::string("–"); }
                );
                button.addChild(valueCell(containerStateText(container.state), scale));
              },
      }));
    }
  }
  if (containers.empty()) {
    list.addChild(text(i18n::tr("activity.containers-empty"), Style::fontSizeBody * scale, ColorRole::Label2));
  }
}

void ActivityWindow::addContainerInspector(Flex& inspector, const ContainerInfo& container, float scale) {
  const auto engine = std::ranges::find(m_containers->engines(), container.engineSocket, &ContainerEngine::socket);
  const std::string engineName = engine != m_containers->engines().end() ? engine->name : container.engineSocket;
  inspector.addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::glyph({
          .glyph = "package",
          .glyphSize = kInspectorIcon * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
          text(container.name, Tokens::typeTitle3.size * scale, ColorRole::Label1, FontWeight::SemiBold),
          ui::label({
              .text = container.image + " · " + engineName,
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .ellipsize = TextEllipsize::Middle,
          })
      )
  ));
  auto facts = card(m_glass, scale);
  const auto fact = [&](std::string name, std::function<std::string(const ContainerInfo&)> textOf) {
    Label* value = nullptr;
    facts->addChild(cardRow(std::move(name), textOf(container), scale, &value));
    m_liveText.push_back({value, [this, id = container.id, textOf = std::move(textOf)] {
                            const ContainerInfo* live = m_containers->find(id);
                            return live != nullptr ? textOf(*live) : std::string();
                          }});
  };
  fact(i18n::tr("activity.column-state"), [](const ContainerInfo& c) { return containerStateText(c.state); });
  fact(i18n::tr("activity.column-cpu"), [](const ContainerInfo& c) {
    return hasUsage(c) ? cpuText(c.cpuPercent) : std::string("–");
  });
  fact(i18n::tr("activity.column-memory"), [](const ContainerInfo& c) {
    return hasUsage(c) ? memoryText(c.memoryBytes) : std::string("–");
  });
  fact(i18n::tr("activity.fact-status"), [](const ContainerInfo& c) { return c.status; });
  settings::addSettingsRowSeparators(*facts, scale);
  inspector.addChild(std::move(facts));
  inspector.addChild(ui::spacer());
  if (!m_actionError.empty()) {
    inspector.addChild(ui::label({
        .text = m_actionError,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Warning),
        .maxLines = 3,
    }));
  }
  auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale});
  const auto action = [&](const char* key, ContainerService::Action kind) {
    buttons->addChild(actionButton(i18n::tr(key), scale, [this, id = container.id, kind] {
      if (const ContainerInfo* live = m_containers->find(id); live != nullptr) {
        runContainerAction(*live, static_cast<int>(kind));
      }
    }));
  };
  if (container.state == "running") {
    action("activity.action-pause", ContainerService::Action::Pause);
  } else if (container.state == "paused") {
    action("activity.action-resume", ContainerService::Action::Unpause);
  }
  if (hasUsage(container) || container.state == "restarting") {
    action("activity.action-stop", ContainerService::Action::Stop);
  }
  inspector.addChild(std::move(buttons));
}
