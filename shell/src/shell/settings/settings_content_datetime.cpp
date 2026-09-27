#include "shell/settings/settings_content_datetime.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <optional>

namespace settings {

  namespace {

    constexpr float kSelectWidth = 260.0F;

    std::unique_ptr<Flex> makeRow(std::string_view title, std::unique_ptr<Node> control, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      row->addChild(ui::spacer());
      row->addChild(std::move(control));
      return row;
    }

    std::string formatUtc(std::int64_t usec) {
      const std::time_t seconds = static_cast<std::time_t>(usec / 1'000'000);
      std::tm tm{};
      gmtime_r(&seconds, &tm);
      return std::format(
          "{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
          tm.tm_sec
      );
    }

  } // namespace

  void addSettingsDateTime(Flex& content, const SettingsDateTimeContext& ctx) {
    if (ctx.timedate == nullptr || !ctx.timedate->ready()) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.datetime.unavailable"), ctx.scale));
      return;
    }
    const TimeDateService& td = *ctx.timedate;
    Flex* body = addSettingsCard(content, i18n::tr("settings.datetime.title"), ctx.scale);

    body->addChild(makeRow(
        i18n::tr("settings.datetime.automatic"),
        ui::toggle({.checked = td.ntp(), .scale = ctx.scale, .onChange = [set = ctx.setNtp](bool v) { set(v); }}),
        ctx.scale
    ));

    const auto& zones = td.timezones();
    const auto zoneIt = std::ranges::find(zones, td.timezone());
    body->addChild(makeRow(
        i18n::tr("settings.datetime.timezone"),
        ui::select({
            .options = zones,
            .selectedIndex = zoneIt != zones.end() ? std::optional<std::size_t>(zoneIt - zones.begin()) : std::nullopt,
            .fontSize = Style::fontSizeBody * ctx.scale,
            .controlHeight = Style::controlHeight * ctx.scale,
            .glyphSize = Style::fontSizeBody * ctx.scale,
            .width = kSelectWidth * ctx.scale,
            .height = Style::controlHeight * ctx.scale,
            .onSelectionChanged = [setTimezone = ctx.setTimezone,
                                   zones](std::size_t index, std::string_view) { setTimezone(zones[index]); },
        }),
        ctx.scale
    ));

    if (!td.ntp()) {
      body->addChild(makeRow(
          i18n::tr("settings.datetime.set-time"),
          ui::input({
              .value = formatUtc(td.timeUsec()),
              .placeholder = std::string("YYYY-MM-DD HH:MM:SS"),
              .fontSize = Style::fontSizeBody * ctx.scale,
              .controlHeight = Style::controlHeight * ctx.scale,
              .width = kSelectWidth * ctx.scale,
              .onSubmit = [set = ctx.setTimeText](const std::string& text) { set(text); },
          }),
          ctx.scale
      ));
    }

    body->addChild(makeRow(
        i18n::tr("settings.datetime.local-rtc"),
        ui::toggle(
            {.checked = td.localRtc(), .scale = ctx.scale, .onChange = [set = ctx.setLocalRtc](bool v) { set(v); }}
        ),
        ctx.scale
    ));

    if (!td.lastError().empty()) {
      body->addChild(makeLabel(td.lastError(), Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error)));
    }
  }

} // namespace settings
