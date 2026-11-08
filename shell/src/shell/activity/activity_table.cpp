#include "shell/activity/activity_ui.h"
#include "shell/activity/activity_window.h"

#include "dbus/upower/upower_service.h"
#include "render/scene/accessible.h"
#include "shell/settings/settings_selection_style.h"
#include "core/deferred_call.h"
#include "system/activity_monitor.h"
#include "system/battery_impact.h"
#include "system/system_monitor_service.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"

#include <ctime>

using namespace activity_ui;

namespace {

  constexpr double kPressureHigh = 10.0; // avg10 %: apps waited for memory a tenth of the time
  constexpr double kRamHigh = 90.0;
  constexpr double kCpuBusy = 90.0;
  constexpr std::size_t kCpuBusySamples = 10;

  std::uint64_t nowUsec() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count()
    );
  }

  std::string percentText(double value) { return std::format("{:.0f} %", value); }

  std::string upTimeText() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    const auto minutes = ts.tv_sec / 60;
    return std::format("{} h {} m", minutes / 60, minutes % 60);
  }

  // Strip chip (mockup .chipk): 20 high capsule, an 8 colour key, caption text.
  std::unique_ptr<Flex> chip(Label** out, std::string text, ColorRole key, bool warn, float scale) {
    auto pill = ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Style::spaceSm * scale,
        .fill = warn ? colorSpecFromRole(ColorRole::Warning, 0.14F) : colorSpecFromRole(ColorRole::Fill1),
        .radius = Tokens::spaceP20 * scale / 2.0F,
        .height = Tokens::spaceP20 * scale,
    });
    if (!warn) {
      pill->addChild(ui::box({
          .fill = colorSpecFromRole(key),
          .radius = Tokens::spaceP2 * scale,
          .width = Tokens::spaceP8 * scale,
          .height = Tokens::spaceP8 * scale,
      }));
    }
    pill->addChild(ui::label({
        .out = out,
        .text = std::move(text),
        .fontSize = Tokens::typeCaption.size * scale,
        .fontWeight = FontWeight::Medium,
        .fontFeatures = std::string("tnum"),
        .color = colorSpecFromRole(warn ? ColorRole::Warning : ColorRole::Label1),
    }));
    return pill;
  }

} // namespace

std::vector<ActivityWindow::Column> ActivityWindow::columnsFor(Pane pane) const {
  const Column cpu{
      .header = i18n::tr("activity.column-cpu"),
      .text = [](const ActivityRow& r) { return cpuText(r.cpuPercent); },
      .value = [](const ActivityRow& r) { return r.cpuPercent; },
      .group = 'c',
  };
  const Column memory{
      .header = i18n::tr("activity.column-memory"),
      .text = [](const ActivityRow& r) { return memoryText(r.memoryBytes); },
      .value = [](const ActivityRow& r) { return static_cast<double>(r.memoryBytes); },
      .group = 'm',
  };
  const Column gpu{
      .header = i18n::tr("activity.column-gpu"),
      .text = [this](const ActivityRow& r) { return percentText(gpuOf(r)); },
      .value = [this](const ActivityRow& r) { return gpuOf(r); },
      .muted = true,
  };
  const Column running{
      .header = i18n::tr("activity.column-running"),
      .text = [](const ActivityRow& r) { return uptimeText(r.startedUsec); },
      .value = [](const ActivityRow& r) {
        return r.startedUsec == 0 ? 0.0 : static_cast<double>(nowUsec() - std::min(nowUsec(), r.startedUsec));
      },
      .muted = true,
  };
  const Column energy{
      .header = i18n::tr("activity.column-energy"),
      .text = [this](const ActivityRow& r) { return energyText(energyBars(r.cpuPercent, gpuOf(r))); },
      .value = [this](const ActivityRow& r) {
        return energyBars(r.cpuPercent, gpuOf(r)) * 1000.0 + r.cpuPercent + gpuOf(r);
      },
  };
  const auto muted = [](Column column) {
    column.muted = true;
    return column;
  };
  switch (pane) {
  case Pane::Cpu:
    return {
        cpu,
        {.header = i18n::tr("activity.column-cpu-time"),
         .text = [](const ActivityRow& r) { return clockText(r.cpuUsageUsec); },
         .value = [](const ActivityRow& r) { return static_cast<double>(r.cpuUsageUsec); },
         .muted = true},
        gpu,
        running,
    };
  case Pane::Memory:
    return {
        memory,
        cpu,
        muted(energy),
        {.header = i18n::tr("activity.column-pressure"),
         .text = [](const ActivityRow& r) { return percentText(r.memoryPressure); },
         .value = [](const ActivityRow& r) { return r.memoryPressure; },
         .muted = true},
        {.header = i18n::tr("activity.column-user"),
         .text = [](const ActivityRow& r) {
           return i18n::tr(r.systemManager ? "activity.user-system" : "activity.user-you");
         },
         .value = [](const ActivityRow& r) { return r.systemManager ? 1.0 : 0.0; },
         .muted = true},
    };
  case Pane::Energy:
    return {energy, cpu, gpu, running};
  case Pane::Disk:
    return {
        {.header = i18n::tr("activity.column-read"),
         .text = [](const ActivityRow& r) { return rateText(r.ioReadBytesPerSec); },
         .value = [](const ActivityRow& r) { return r.ioReadBytesPerSec; }},
        {.header = i18n::tr("activity.column-written"),
         .text = [](const ActivityRow& r) { return rateText(r.ioWriteBytesPerSec); },
         .value = [](const ActivityRow& r) { return r.ioWriteBytesPerSec; }},
        muted(cpu),
        muted(memory),
    };
  case Pane::Network:
  case Pane::Containers:
    return {
        {.header = i18n::tr("activity.column-network"),
         .text = [this](const ActivityRow& r) {
           const auto rate = m_details != nullptr ? m_details->networkBytesPerSec(r.key) : std::nullopt;
           return rate.has_value() ? rateText(*rate) : std::string("–");
         },
         .value = [this](const ActivityRow& r) {
           return m_details != nullptr ? m_details->networkBytesPerSec(r.key).value_or(0.0) : 0.0;
         }},
        muted(cpu),
        muted(memory),
    };
  }
  return {};
}

std::size_t ActivityWindow::sortColumn() const {
  const auto it = m_sortBy.find(m_pane);
  return it != m_sortBy.end() ? it->second : 0;
}

void ActivityWindow::computeOrder() {
  m_orderAt = std::chrono::steady_clock::now();
  m_order.clear();
  if (m_monitor == nullptr || m_pane == Pane::Containers) {
    return;
  }
  const auto columns = columnsFor(m_pane);
  const auto& value = columns[std::min(sortColumn(), columns.size() - 1)].value;
  std::vector<std::pair<int, const ActivityRow*>> rows;
  for (const auto& row : m_monitor->activity().rows()) {
    if (matchesQuery(row)) {
      rows.emplace_back(row.systemManager ? 2 : row.hasWindow ? 0 : 1, &row);
    }
  }
  std::vector<double> values;
  values.reserve(rows.size());
  for (const auto& [section, row] : rows) {
    values.push_back(value(*row));
  }
  std::vector<std::size_t> index(rows.size());
  for (std::size_t i = 0; i < index.size(); ++i) {
    index[i] = i;
  }
  std::ranges::stable_sort(index, [&](std::size_t a, std::size_t b) {
    if (rows[a].first != rows[b].first) {
      return rows[a].first < rows[b].first;
    }
    return values[a] > values[b];
  });
  for (const std::size_t i : index) {
    m_order.push_back(rows[i].second->key);
  }
}

ActivityWindow::Problem ActivityWindow::problem(std::string* rowKey) const {
  const auto& rows = m_monitor->activity().rows();
  const auto top = [&](auto value) -> const ActivityRow* {
    const ActivityRow* best = nullptr;
    for (const auto& row : rows) {
      if (!row.systemManager && (best == nullptr || value(row) > value(*best))) {
        best = &row;
      }
    }
    return best;
  };
  const double pressure = m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back();
  const double ram = m_sysmon != nullptr ? m_sysmon->latest().ramUsagePercent : 0.0;
  if (pressure >= kPressureHigh || ram >= kRamHigh) {
    if (const ActivityRow* row = top([](const ActivityRow& r) { return r.memoryBytes; }); row != nullptr) {
      *rowKey = row->key;
      return Problem::Memory;
    }
  }
  const auto cpuBusy = [this] {
    const auto stats = m_sysmon->history();
    return stats.size() >= kCpuBusySamples
        && std::all_of(stats.end() - kCpuBusySamples, stats.end(), [](const SystemStats& s) {
             return s.cpuUsagePercent >= kCpuBusy;
           });
  };
  if (m_sysmon != nullptr && m_sysmon->latest().cpuUsagePercent >= kCpuBusy && cpuBusy()) {
    if (const ActivityRow* row = top([](const ActivityRow& r) { return r.cpuPercent; }); row != nullptr) {
      *rowKey = row->key;
      return Problem::Cpu;
    }
  }
  for (const auto& row : rows) {
    if (needsAttention(row)) {
      *rowKey = row.key;
      return Problem::Background;
    }
  }
  return Problem::None;
}

std::unique_ptr<Flex> ActivityWindow::buildToolbar(float scale) {
  auto bar = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .height = kToolbarHeight * scale,
      .configure = [scale](Flex& flex) { flex.setPadding(0.0F, Style::spaceMd * scale, 0.0F, Style::spaceLg * scale); },
  });
  Label* count = nullptr;
  bar->addChild(ui::column(
      {.align = FlexAlign::Start},
      text(i18n::tr("activity.title"), Tokens::typeTitle3.size * scale, ColorRole::Label1, FontWeight::SemiBold),
      text(
          i18n::tr("activity.count", "count", std::to_string(m_monitor->activity().rows().size())),
          Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium, &count
      )
  ));
  m_liveText.push_back({count, [this] {
                          return i18n::tr("activity.count", "count", std::to_string(m_monitor->activity().rows().size()));
                        }});
  bar->addChild(ui::spacer());

  std::vector<ui::SegmentedOption> panes;
  for (const char* key : {"activity.pane-cpu", "activity.pane-memory", "activity.pane-energy", "activity.pane-disk",
                          "activity.pane-network"}) {
    panes.push_back({.label = i18n::tr(key)});
  }
  if (containersAvailable()) {
    panes.push_back({.label = i18n::tr("activity.tab-containers")});
  }
  bar->addChild(ui::segmented({
      .options = std::move(panes),
      .selectedIndex = static_cast<std::size_t>(m_pane),
      .fontSize = Style::fontSizeBody * scale,
      .scale = scale,
      .onChange = [this](std::size_t index) { setPane(static_cast<Pane>(index)); },
  }));
  bar->addChild(ui::spacer());

  if (m_keptSearch != nullptr && m_searchScale == scale) {
    m_searchInput = static_cast<Input*>(bar->addChild(std::move(m_keptSearch)));
  } else {
    m_keptSearch.reset();
    m_searchScale = scale;
    bar->addChild(ui::input({
        .out = &m_searchInput,
        .value = m_query,
        .placeholder = i18n::tr("activity.search"),
        .fontSize = Style::fontSizeBody * scale,
        .width = kSearchWidth * scale,
        .height = Style::controlHeight * scale,
        .onChange = [this](const std::string& query) {
          m_query = query;
          computeOrder();
          requestRebuild();
        },
    }));
  }
  if (Style::closeButtonsEnabled()) {
    bar->addChild(ui::button({
        .glyph = "close",
        .glyphSize = Tokens::sizeSymbolInline * scale,
        .variant = ButtonVariant::Default,
        .minWidth = Tokens::sizeControlS * scale,
        .minHeight = Tokens::sizeControlS * scale,
        .padding = 0.0F,
        .radius = Tokens::sizeControlS * 0.5F * scale,
        .onClick = [this] { DeferredCall::callLater([this] { close(); }); },
        .configure = [](Button& button) {
          button.setTabStop(false);
          button.accessibleState().name = i18n::tr("activity.close");
        },
    }));
  }
  return bar;
}

std::unique_ptr<Flex> ActivityWindow::buildProblemCard(float scale) {
  std::string key;
  const Problem kind = problem(&key);
  const ActivityRow* row = kind != Problem::None ? findRow(key) : nullptr;
  if (row == nullptr) {
    return nullptr;
  }
  const std::string name = identity(*row).name;
  std::string title;
  std::string action;
  std::function<std::string()> body;
  switch (kind) {
  case Problem::Memory:
    title = i18n::tr("activity.problem-memory-title");
    action = i18n::tr("activity.action-quit-app", "name", name);
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      if (live == nullptr) {
        return std::string();
      }
      const double pressure = m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back();
      return i18n::tr(
          "activity.problem-memory-body", "percent", std::format("{:.0f}", pressure), "name", identity(*live).name,
          "memory", memoryText(live->memoryBytes)
      );
    };
    break;
  case Problem::Cpu:
    title = i18n::tr("activity.problem-cpu-title");
    action = i18n::tr("activity.action-show-app", "name", name);
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      return live != nullptr ? i18n::tr("activity.problem-cpu-body", "name", identity(*live).name, "cpu",
                                        cpuText(live->cpuPercent))
                             : std::string();
    };
    break;
  case Problem::Background: {
    std::size_t count = 0;
    for (const auto& r : m_monitor->activity().rows()) {
      count += needsAttention(r) ? 1 : 0;
    }
    title = count == 1 ? i18n::tr("activity.problem-background-one", "name", name)
                       : i18n::tr("activity.problem-background-many", "count", std::to_string(count));
    action = i18n::tr("activity.action-review");
    body = [this, key] {
      const ActivityRow* live = findRow(key);
      const auto* state = rowState(key);
      return live != nullptr && state != nullptr ? BackgroundActivityMonitor::reasonText(*state, live->memoryBytes)
                                                 : std::string();
    };
    break;
  }
  case Problem::None:
    return nullptr;
  }

  Label* bodyLabel = nullptr;
  auto card = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
      .paddingV = Style::spaceMd * scale,
      .paddingH = Style::spaceLg * scale,
      .fill = m_glass ? colorSpecFromRole(ColorRole::SolidThin, 0.5F)
                      : fixedColorSpec(rgbaHex(isResolvedLightTheme() ? Tokens::schemeLight.card : Tokens::schemeDark.card)),
      .radius = Tokens::radiusR8 * scale,
      .border = colorSpecFromRole(m_glass ? ColorRole::LensEdge : ColorRole::Stroke),
      .borderWidth = 0.5F * scale,
  });
  card->addChild(ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .fill = colorSpecFromRole(ColorRole::Warning, 0.18F),
       .radius = Tokens::sizeControlM * scale / 2.0F,
       .width = Tokens::sizeControlM * scale,
       .height = Tokens::sizeControlM * scale},
      ui::glyph({
          .glyph = "alert-triangle",
          .glyphSize = Tokens::sizeSymbol * scale,
          .color = colorSpecFromRole(ColorRole::Warning),
      })
  ));
  auto words = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  words->addChild(text(title, Tokens::typeHeadline.size * scale, ColorRole::Label1, FontWeight::SemiBold));
  words->addChild(ui::label({
      .out = &bodyLabel,
      .text = body(),
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::Medium,
      .color = colorSpecFromRole(ColorRole::Label2),
      .maxLines = 2,
      .ellipsize = TextEllipsize::End,
  }));
  card->addChild(std::move(words));
  m_liveText.push_back({bodyLabel, std::move(body)});
  card->addChild(ui::button({
      .text = action,
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Tokens::radiusR8 * scale,
      .onClick = [this, key, kind] {
        select(key);
        if (kind == Problem::Memory) {
          m_confirm = Confirm::Quit;
        }
      },
  }));
  auto wrap = ui::column({.align = FlexAlign::Stretch});
  wrap->setPadding(0.0F, Style::spaceLg * scale, Style::spaceSm * scale, Style::spaceLg * scale);
  wrap->addChild(std::move(card));
  return wrap;
}

std::unique_ptr<Button> ActivityWindow::buildRow(
    Renderer& renderer, const ActivityRow& row, const std::vector<Column>& columns, float nameWidth, float scale
) {
  const RowIdentity id = identity(row);
  const bool selected = row.key == m_selectedKey;
  const int iconPx = static_cast<int>(std::round(Tokens::sizeSymbol * scale));
  Image* icon = nullptr;
  auto image = ui::image({.out = &icon, .width = Tokens::sizeSymbol * scale, .height = Tokens::sizeSymbol * scale});
  if (const std::string path = iconPath(id.icon, iconPx); !path.empty()) {
    icon->setSourceFile(renderer, path, iconPx);
  }
  const std::size_t sortIndex = sortColumn();
  return ui::button({
      .contentAlign = ButtonContentAlign::Start,
      .variant = ButtonVariant::Tab,
      .customPalette = selected ? std::optional{settings::selectedRowPalette(
                                      settings::selectionStyle().rowFill, settings::selectionStyle().rowLabel
                                  )}
                                : std::nullopt,
      .minHeight = kRowHeight * scale,
      .padding = 0.0F,
      .gap = 0.0F,
      .radius = Tokens::radiusR4 * scale,
      .onClick = [this, key = row.key] { select(key); },
      .configure =
          [&](Button& button) {
            button.setTabStop(false);
            button.accessibleState().name = id.name;
            button.accessibleState().description = captionFor(row);
            m_liveCalls.push_back([this, key = row.key, target = &button] {
              if (const ActivityRow* live = findRow(key); live != nullptr) {
                target->accessibleState().description = captionFor(*live);
              }
            });
            auto name = ui::row({
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * scale,
                .paddingH = Style::spaceMd * scale,
                .width = nameWidth,
                .height = kRowHeight * scale,
            });
            if (m_details != nullptr) {
              const bool open = m_details->expanded(row.key);
              name->addChild(ui::button({
                  .glyph = open ? "chevron-down" : "chevron-right",
                  .glyphSize = Tokens::sizeSymbolInline * scale,
                  .variant = ButtonVariant::Ghost,
                  .padding = 0.0F,
                  .width = Tokens::sizeSymbol * scale,
                  .height = Tokens::sizeSymbol * scale,
                  .onClick = [this, key = row.key, open] {
                    m_details->setExpanded(key, !open); // groups fill in on the next sample
                    requestRebuild();
                  },
                  .configure = [](Button& disclosure) { disclosure.setTabStop(false); },
              }));
            }
            name->addChild(std::move(image));
            name->addChild(ui::label({
                .text = id.name,
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::Label1),
                .maxLines = 1,
                .ellipsize = TextEllipsize::Middle,
                .flexGrow = 1.0F,
            }));
            if (const auto* groups = m_details != nullptr && m_details->expanded(row.key) ? m_details->groups(row.key)
                                                                                          : nullptr;
                groups != nullptr) {
              int processes = 0;
              for (const auto& group : *groups) {
                processes += group.count;
              }
              name->addChild(text(std::to_string(processes), Style::fontSizeCaption * scale, ColorRole::Label3));
            }
            button.addChild(std::move(name));
            for (std::size_t i = 0; i < columns.size(); ++i) {
              const Column& column = columns[i];
              Flex* cellRaw = nullptr;
              Label* label = nullptr;
              button.addChild(ui::row(
                  {.out = &cellRaw,
                   .align = FlexAlign::Center,
                   .justify = FlexJustify::End,
                   .paddingH = Style::spaceMd * scale,
                   .width = m_columnWidth,
                   .height = kRowHeight * scale},
                  ui::label({
                      .out = &label,
                      .text = column.text(row),
                      .fontSize = Style::fontSizeBody * scale,
                      .fontFeatures = std::string("tnum"),
                      .color = colorSpecFromRole(column.muted && i != sortIndex ? ColorRole::Label2 : ColorRole::Label1),
                      .maxLines = 1,
                      .textAlign = TextAlign::End,
                  })
              ));
              m_liveText.push_back({label, [this, key = row.key, text = column.text] {
                                      const ActivityRow* live = findRow(key);
                                      return live != nullptr ? text(*live) : std::string();
                                    }});
              if (i == sortIndex) {
                m_liveHeat.push_back({cellRaw, [this, key = row.key, value = column.value] {
                                        const ActivityRow* live = findRow(key);
                                        return live != nullptr && m_heatTop > 0.0
                                            ? static_cast<float>(std::clamp(value(*live) / m_heatTop, 0.0, 1.0))
                                            : 0.0F;
                                      }});
                cellRaw->setFill(colorSpecFromRole(ColorRole::Accent, m_liveHeat.back().share() * kHeatAlpha));
              }
            }
            std::string glyph;
            ColorRole role = ColorRole::Label3;
            const Limits limits = m_limitedKeys.contains(row.key) ? limitsOf(row) : Limits{};
            if (needsAttention(row)) {
              glyph = "alert-triangle";
              role = ColorRole::Warning;
            } else if (m_limitedKeys.contains(row.key) && isFrozen(row)) {
              glyph = "player-pause";
            } else if (limits.cpu != 0 || limits.memory != 0) {
              glyph = "leaf";
              role = ColorRole::Success;
            }
            auto status = ui::row(
                {.align = FlexAlign::Center,
                 .justify = FlexJustify::End,
                 .paddingH = Style::spaceSm * scale,
                 .width = kStatusWidth * scale,
                 .height = kRowHeight * scale}
            );
            if (!glyph.empty()) {
              status->addChild(ui::glyph({
                  .glyph = glyph,
                  .glyphSize = Tokens::sizeSymbolInline * scale,
                  .color = colorSpecFromRole(role),
              }));
            }
            button.addChild(std::move(status));
          },
  });
}

void ActivityWindow::addChildRows(
    Flex& list, const ActivityRow& row, const std::vector<Column>& columns, float nameWidth, float scale
) {
  const auto* groups = m_details != nullptr && m_details->expanded(row.key) ? m_details->groups(row.key) : nullptr;
  if (groups == nullptr) {
    return;
  }
  for (const auto& group : *groups) {
    auto child = ui::row({.align = FlexAlign::Center, .height = kRowHeight * scale});
    auto name = ui::row({.align = FlexAlign::Center, .width = nameWidth, .height = kRowHeight * scale});
    name->setPadding(0.0F, Style::spaceMd * scale, 0.0F, kChildIndent * scale);
    name->addChild(ui::label({
        .text = group.count > 1 ? std::format("{} ({})", group.command, group.count) : group.command,
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 1,
        .ellipsize = TextEllipsize::Middle,
        .flexGrow = 1.0F,
    }));
    child->addChild(std::move(name));
    for (const Column& column : columns) {
      Label* label = nullptr;
      const auto value = [kind = column.group](const ProcessGroupUsage& g) {
        return kind == 'c' ? cpuText(g.cpuPercent) : kind == 'm' ? memoryText(g.residentBytes) : std::string();
      };
      child->addChild(ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::End,
           .paddingH = Style::spaceMd * scale,
           .width = m_columnWidth,
           .height = kRowHeight * scale},
          ui::label({
              .out = &label,
              .text = value(group),
              .fontSize = Style::fontSizeBody * scale,
              .fontFeatures = std::string("tnum"),
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .textAlign = TextAlign::End,
          })
      ));
      if (column.group != 0) {
        m_liveText.push_back({label, [this, key = row.key, command = group.command, value] {
                                const auto* live = m_details != nullptr ? m_details->groups(key) : nullptr;
                                if (live == nullptr) {
                                  return std::string();
                                }
                                const auto it = std::ranges::find(*live, command, &ProcessGroupUsage::command);
                                return it != live->end() ? value(*it) : std::string();
                              }});
      }
    }
    child->addChild(ui::node({.width = kStatusWidth * scale}));
    list.addChild(std::move(child));
  }
}

std::unique_ptr<Flex> ActivityWindow::buildTable(Renderer& renderer, float width, float scale) {
  auto table = ui::column({.align = FlexAlign::Stretch, .paddingH = Style::spaceSm * scale, .flexGrow = 1.0F});
  auto columns = columnsFor(m_pane);
  // Columns drop from the right until the name keeps its minimum width.
  const float inner = width - 2.0F * Style::spaceSm * scale - kStatusWidth * scale;
  while (columns.size() > 1 && inner - static_cast<float>(columns.size()) * kColumnWidth * scale < kNameMinWidth * scale) {
    columns.pop_back();
  }
  // Past the name's comfortable width, the value columns take the extra room (grow then clamp).
  const auto count = static_cast<float>(columns.size());
  m_columnWidth = std::clamp((inner - kNameMaxWidth * scale) / count, kColumnWidth * scale, kColumnMaxWidth * scale);
  const float nameWidth = inner - count * m_columnWidth;
  const std::size_t sortIndex = sortColumn();

  auto header = ui::row({.align = FlexAlign::Center, .height = kRowHeight * scale});
  header->addChild(ui::row(
      {.align = FlexAlign::Center, .paddingH = Style::spaceMd * scale, .width = nameWidth},
      text(i18n::tr("activity.column-name"), Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium)
  ));
  for (std::size_t i = 0; i < columns.size(); ++i) {
    const bool sorted = i == sortIndex;
    header->addChild(ui::button({
        .text = sorted ? columns[i].header + " ▾" : columns[i].header,
        .fontSize = Style::fontSizeCaption * scale,
        .contentAlign = ButtonContentAlign::End,
        .variant = ButtonVariant::Ghost,
        .customPalette = Button::ButtonPalette{
            .normal = {.bg = clearColorSpec(), .border = clearColorSpec(),
                       .label = colorSpecFromRole(sorted ? ColorRole::Label1 : ColorRole::Label2)},
            .hover = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label1)},
            .pressed = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label1)},
            .disabled = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label3)},
        },
        .paddingV = 0.0F,
        .paddingH = Style::spaceMd * scale,
        .width = m_columnWidth,
        .height = kRowHeight * scale,
        .onClick = [this, i] {
          m_sortBy[m_pane] = i;
          computeOrder();
          requestRebuild();
        },
        .configure = [](Button& button) { button.setTabStop(false); },
    }));
  }
  header->addChild(ui::node({.width = kStatusWidth * scale}));
  table->addChild(std::move(header));
  table->addChild(ui::separator({}));

  const bool sampled = m_monitor->activity().sampled();
  if (!sampled && std::chrono::steady_clock::now() - m_openedAt < kSkeletonFor) {
    for (int i = 0; i < kSkeletonRows; ++i) {
      table->addChild(ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .paddingH = Style::spaceMd * scale,
           .height = kRowHeight * scale},
          ui::box({.fill = colorSpecFromRole(ColorRole::Fill2), .radius = Tokens::radiusR4 * scale,
                   .width = Tokens::sizeSymbol * scale, .height = Tokens::sizeSymbol * scale}),
          ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR4 * scale,
                   .width = nameWidth / static_cast<float>(2 + i % 3), .height = Tokens::spaceP12 * scale})
      ));
    }
    return table;
  }
  const auto centered = [&](std::unique_ptr<Node> first, std::unique_ptr<Node> second = nullptr) {
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * scale,
                           .flexGrow = 1.0F});
    box->addChild(std::move(first));
    if (second != nullptr) {
      box->addChild(std::move(second));
    }
    table->addChild(std::move(box));
  };
  if (!sampled) {
    centered(text(i18n::tr("activity.loading"), Style::fontSizeBody * scale, ColorRole::Label2));
    return table;
  }
  if (m_order.empty()) {
    if (m_query.empty()) {
      centered(text(i18n::tr("activity.empty"), Style::fontSizeBody * scale, ColorRole::Label2));
    } else {
      centered(
          text(i18n::tr("activity.empty-search", "query", m_query), Style::fontSizeBody * scale, ColorRole::Label2),
          ui::button({
              .text = i18n::tr("activity.action-clear"),
              .fontSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minHeight = Style::controlHeight * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Tokens::radiusR8 * scale,
              .onClick = [this] {
                m_query.clear();
                if (m_searchInput != nullptr) {
                  m_searchInput->setValue("");
                }
                computeOrder();
                requestRebuild();
              },
          })
      );
    }
    return table;
  }

  ScrollView* scroll = nullptr;
  table->addChild(ui::scrollView({
      .out = &scroll,
      .state = &m_listScroll,
      .scrollbarAutoHide = true,
      .flexGrow = 1.0F,
  }));
  Flex& list = *scroll->content();
  list.setAlign(FlexAlign::Stretch);
  list.setGap(0.0F);
  if (sortIndex < columns.size()) {
    m_liveHeat.push_back({nullptr, [this, value = columns[sortIndex].value] {
                            m_heatTop = 0.0;
                            for (const auto& key : m_order) {
                              if (const ActivityRow* row = findRow(key); row != nullptr) {
                                m_heatTop = std::max(m_heatTop, value(*row));
                              }
                            }
                            return 0.0F;
                          }});
    m_liveHeat.front().share();
  }
  static constexpr std::array<const char*, 3> kSections{"activity.group-apps", "activity.tab-background",
                                                        "activity.group-system"};
  int section = -1;
  for (const auto& key : m_order) {
    const ActivityRow* row = findRow(key);
    if (row == nullptr) {
      continue;
    }
    const int rowSection = row->systemManager ? 2 : row->hasWindow ? 0 : 1;
    if (rowSection != section) {
      section = rowSection;
      list.addChild(ui::row(
          {.align = FlexAlign::End, .paddingH = Style::spaceMd * scale, .height = kRowHeight * scale},
          text(i18n::tr(kSections[static_cast<std::size_t>(section)]), Style::fontSizeCaption * scale, ColorRole::Label2,
               FontWeight::SemiBold)
      ));
    }
    list.addChild(buildRow(renderer, *row, columns, nameWidth, scale));
    addChildRows(list, *row, columns, nameWidth, scale);
  }
  return table;
}

std::unique_ptr<Flex> ActivityWindow::buildStrip(float width, float scale) {
  auto strip = ui::row({
      .align = FlexAlign::Start,
      .gap = Style::spaceLg * scale + Style::spaceSm * scale,
      .paddingV = Style::spaceLg * scale,
      .paddingH = Tokens::spaceP24 * scale,
      .height = kStripHeight * scale,
  });
  const float gridWidth = (m_pane == Pane::Cpu ? kStripGridCpuWidth : kStripGridWidth) * scale;
  const float graphWidth = std::max(
      Tokens::spaceP64 * scale, width - 2.0F * Tokens::spaceP24 * scale - Tokens::spaceP24 * scale - gridWidth
  );
  const auto stats = [this] { return m_sysmon != nullptr ? m_sysmon->history() : std::vector<SystemStats>{}; };
  const auto last = [this] { return m_sysmon != nullptr ? m_sysmon->latest() : SystemStats{}; };
  const auto series = [stats](auto pick, float floor) {
    return [stats, pick, floor] {
      std::vector<float> values;
      for (const auto& sample : stats()) {
        values.push_back(static_cast<float>(pick(sample)));
      }
      return normalized(values, floor);
    };
  };
  const UPowerService* upower = m_upowerProvider ? m_upowerProvider() : nullptr;
  const bool battery = upower != nullptr && upower->state().isPresent;

  struct Cell {
    std::string label;
    std::function<std::string()> value;
  };
  std::string caption;
  std::vector<std::tuple<std::function<std::string()>, ColorRole, bool>> chips;
  std::function<std::vector<float>()> values;
  ColorRole graphRole = ColorRole::Accent;
  std::vector<Cell> cells;
  const auto rowsSum = [this](auto pick) {
    double sum = 0.0;
    for (const auto& row : m_monitor->activity().rows()) {
      sum += pick(row);
    }
    return sum;
  };
  const auto peak = [](const std::deque<float>& history) {
    return history.empty() ? 0.0F : *std::ranges::max_element(history);
  };
  switch (m_pane) {
  case Pane::Cpu:
    caption = i18n::tr("activity.strip-cpu");
    chips.emplace_back([last] { return i18n::tr("activity.chip-cpu", "value", percentText(last().cpuUsagePercent)); },
                       ColorRole::Accent, false);
    values = series([](const SystemStats& s) { return s.cpuUsagePercent; }, 100.0F);
    cells = {
        {i18n::tr("activity.grid-idle"), [last] { return percentText(100.0 - last().cpuUsagePercent); }},
        {i18n::tr("activity.grid-load"), [last] { return std::format("{:.2f}", last().loadAvg1); }},
        {i18n::tr("activity.grid-running"), [this] { return std::to_string(m_monitor->activity().rows().size()); }},
        {i18n::tr("activity.grid-speed"),
         [last] {
           const SystemStats s = last();
           return s.cpuFreqAvailable ? std::format("{:.1f} GHz", s.cpuFreqMhz / 1000.0) : std::string("–");
         }},
        {i18n::tr("activity.grid-temperature"),
         [last] {
           const SystemStats s = last();
           return s.cpuTempC.has_value() ? std::format("{:.0f} °C", *s.cpuTempC) : std::string("–");
         }},
        {i18n::tr("activity.grid-uptime"), [] { return upTimeText(); }},
    };
    break;
  case Pane::Memory: {
    std::string key;
    const bool high = problem(&key) == Problem::Memory;
    caption = i18n::tr("activity.strip-memory");
    if (high) {
      chips.emplace_back([this] {
        const bool rising = m_pressureHistory.size() > 10
            && m_pressureHistory.back() > m_pressureHistory[m_pressureHistory.size() - 11];
        return i18n::tr(rising ? "activity.chip-rising" : "activity.chip-high");
      }, ColorRole::Warning, true);
      graphRole = ColorRole::Warning;
    } else {
      chips.emplace_back([this] {
        return i18n::tr("activity.chip-pressure", "value",
                        percentText(m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back()));
      }, ColorRole::Accent, false);
    }
    values = [this] { return normalized(m_pressureHistory, 20.0F); };
    const auto mb = [](std::uint64_t value) { return memoryText(value * 1024 * 1024); };
    cells = {
        {i18n::tr("activity.grid-physical"), [last, mb] { return mb(last().ramTotalMb); }},
        {i18n::tr("activity.grid-used"), [last, mb] { return mb(last().ramUsedMb); }},
        {i18n::tr("activity.grid-app-memory"),
         [rowsSum] {
           return memoryText(static_cast<std::uint64_t>(rowsSum([](const ActivityRow& r) {
             return static_cast<double>(r.memoryBytes);
           })));
         }},
        {i18n::tr("activity.grid-swap"), [last, mb] { return mb(last().swapUsedMb); }},
        {i18n::tr("activity.grid-swap-size"), [last, mb] { return mb(last().swapTotalMb); }},
        {i18n::tr("activity.column-pressure"),
         [this] { return percentText(m_pressureHistory.empty() ? 0.0 : m_pressureHistory.back()); }},
    };
    break;
  }
  case Pane::Energy:
    caption = i18n::tr("activity.strip-energy");
    if (battery) {
      chips.emplace_back([this] {
        const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
        return i18n::tr("activity.chip-battery", "value",
                        percentText(live != nullptr ? live->state().percentage : 0.0));
      }, ColorRole::Success, false);
      values = [this] { return normalized(m_powerHistory, 10.0F); };
    } else {
      chips.emplace_back([last] { return i18n::tr("activity.chip-cpu", "value", percentText(last().cpuUsagePercent)); },
                         ColorRole::Accent, false);
      values = series([](const SystemStats& s) { return s.cpuUsagePercent; }, 100.0F);
    }
    cells = {
        {i18n::tr("activity.grid-battery"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           return live != nullptr && live->state().isPresent ? percentText(live->state().percentage)
                                                             : i18n::tr("activity.battery-absent");
         }},
        {i18n::tr("activity.grid-power"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           return live != nullptr && live->state().energyRate > 0.0 ? std::format("{:.1f} W", live->state().energyRate)
                                                                    : std::string("–");
         }},
        {i18n::tr("activity.grid-state"),
         [this] {
           const UPowerService* live = m_upowerProvider ? m_upowerProvider() : nullptr;
           if (live == nullptr || !live->state().isPresent) {
             return std::string("–");
           }
           return i18n::tr(live->state().onBattery ? "activity.state-on-battery" : "activity.state-plugged");
         }},
        {i18n::tr("activity.grid-high-energy"),
         [this] {
           int count = 0;
           for (const auto& row : m_monitor->activity().rows()) {
             count += energyBars(row.cpuPercent, gpuOf(row)) >= 3 ? 1 : 0;
           }
           return std::to_string(count);
         }},
        {i18n::tr("activity.column-gpu"),
         [last] {
           const SystemStats s = last();
           return s.gpuUsagePercent.has_value() ? percentText(*s.gpuUsagePercent) : std::string("–");
         }},
        {i18n::tr("activity.column-cpu"), [last] { return percentText(last().cpuUsagePercent); }},
    };
    break;
  case Pane::Disk:
    caption = i18n::tr("activity.strip-disk");
    chips.emplace_back([this] {
      return i18n::tr("activity.chip-total", "value", std::format("{:.1f} MB/s", m_diskHistory.empty() ? 0.0F
                                                                                                        : m_diskHistory.back()));
    }, ColorRole::Accent, false);
    values = [this] { return normalized(m_diskHistory, 1.0F); };
    cells = {
        {i18n::tr("activity.column-read"),
         [rowsSum] { return rateText(rowsSum([](const ActivityRow& r) { return r.ioReadBytesPerSec; })); }},
        {i18n::tr("activity.column-written"),
         [rowsSum] { return rateText(rowsSum([](const ActivityRow& r) { return r.ioWriteBytesPerSec; })); }},
        {i18n::tr("activity.grid-peak"), [this, peak] { return std::format("{:.1f} MB/s", peak(m_diskHistory)); }},
    };
    break;
  case Pane::Network:
  case Pane::Containers:
    caption = i18n::tr("activity.strip-network");
    chips.emplace_back([last] { return i18n::tr("activity.chip-in", "value", rateText(last().netRxBytesPerSec)); },
                       ColorRole::Accent, false);
    chips.emplace_back([last] { return i18n::tr("activity.chip-out", "value", rateText(last().netTxBytesPerSec)); },
                       ColorRole::Label3, false);
    values = series([](const SystemStats& s) { return s.netRxBytesPerSec + s.netTxBytesPerSec; }, 1024.0F * 1024.0F);
    cells = {
        {i18n::tr("activity.grid-received"), [last] { return rateText(last().netRxBytesPerSec); }},
        {i18n::tr("activity.grid-sent"), [last] { return rateText(last().netTxBytesPerSec); }},
        {i18n::tr("activity.grid-peak"),
         [stats] {
           double top = 0.0;
           for (const auto& s : stats()) {
             top = std::max(top, s.netRxBytesPerSec + s.netTxBytesPerSec);
           }
           return rateText(top);
         }},
    };
    break;
  }

  auto graphBox = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .width = graphWidth});
  auto captionRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  captionRow->addChild(text(caption, Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::SemiBold));
  for (auto& [chipText, role, warn] : chips) {
    Label* label = nullptr;
    captionRow->addChild(chip(&label, chipText(), role, warn, scale));
    m_liveText.push_back({label, std::move(chipText)});
  }
  graphBox->addChild(std::move(captionRow));
  auto* graph = static_cast<Graph*>(
      graphBox->addChild(makeGraph(values(), graphWidth, kStripGraphHeight * scale, graphRole))
  );
  m_liveGraphs.push_back({graph, std::move(values)});
  graphBox->addChild(ui::row(
      {.align = FlexAlign::Center, .width = graphWidth},
      text(i18n::tr("activity.time-ago"), Tokens::typeCaption.size * scale, ColorRole::Label3, FontWeight::Medium),
      ui::spacer(),
      text(i18n::tr("activity.time-now"), Tokens::typeCaption.size * scale, ColorRole::Label3, FontWeight::Medium)
  ));
  strip->addChild(std::move(graphBox));

  auto grid = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .width = gridWidth});
  grid->setPadding(Tokens::spaceP24 * scale, 0.0F, 0.0F, 0.0F);
  const float cellWidth = (gridWidth - 2.0F * Style::spaceLg * scale) / 3.0F;
  Flex* line = nullptr;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (i % 3 == 0) {
      line = static_cast<Flex*>(grid->addChild(ui::row({.align = FlexAlign::Start, .gap = Style::spaceLg * scale})));
    }
    Label* value = nullptr;
    line->addChild(ui::column(
        {.align = FlexAlign::Start, .width = cellWidth},
        text(cells[i].label, Style::fontSizeCaption * scale, ColorRole::Label2, FontWeight::Medium),
        ui::label({
            .out = &value,
            .text = cells[i].value(),
            .fontSize = Tokens::typeTitle3.size * scale,
            .fontWeight = FontWeight::SemiBold,
            .fontFeatures = std::string("tnum"),
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 1,
        })
    ));
    m_liveText.push_back({value, std::move(cells[i].value)});
  }
  strip->addChild(std::move(grid));
  return strip;
}
