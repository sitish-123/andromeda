#include "shell/settings/settings_content_datetime.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <langinfo.h>
#include <locale.h>
#include <optional>

namespace settings {

  namespace {

    using namespace page_rows;

    constexpr float kSelectWidth = 240.0F;
    constexpr auto kDatePresets = std::to_array<const char*>({"%A, %x", "%a %e %b", "%e %b", "%a %e %b %Y", "%x"});

    std::string formatUtc(std::int64_t usec) {
      const std::time_t seconds = static_cast<std::time_t>(usec / 1'000'000);
      std::tm tm{};
      gmtime_r(&seconds, &tm);
      return std::format(
          "{:04}-{:02}-{:02} {:02}:{:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
          tm.tm_sec
      );
    }

    std::string formatLocal(const char* format, std::time_t when) {
      std::tm tm{};
      localtime_r(&when, &tm);
      char buf[128]{};
      return std::strftime(buf, sizeof(buf), format, &tm) > 0 ? std::string(buf) : std::string{};
    }

    bool is24Hour(std::string_view format) {
      return format.find("%H") != std::string_view::npos || format.find("%R") != std::string_view::npos
          || format.find("%T") != std::string_view::npos || format.find("%k") != std::string_view::npos;
    }

    // Swaps only the hour and AM/PM tokens, so seconds, separators and the date part survive.
    std::string withHourCycle(std::string format, bool hour24) {
      const auto replaceAll = [&](std::string_view from, std::string_view to) {
        for (std::size_t at = format.find(from); at != std::string::npos; at = format.find(from, at + to.size())) {
          format.replace(at, from.size(), to);
        }
      };
      if (hour24) {
        replaceAll("%r", "%T");
        replaceAll(" %p", "");
        replaceAll(" %P", "");
        replaceAll("%p", "");
        replaceAll("%P", "");
        replaceAll("%I", "%H");
        replaceAll("%l", "%k");
        return format;
      }
      replaceAll("%T", "%I:%M:%S");
      replaceAll("%R", "%I:%M");
      replaceAll("%H", "%I");
      replaceAll("%k", "%l");
      std::size_t last = format.rfind("%M");
      if (const std::size_t seconds = format.rfind("%S"); seconds != std::string::npos
          && (last == std::string::npos || seconds > last)) {
        last = seconds;
      }
      if (last != std::string::npos && format.find("%p") == std::string::npos) {
        format.insert(last + 2, " %p");
      }
      return format;
    }

    // The region localed sets (/etc/locale.conf LC_TIME, else LANG).
    std::string systemRegion() {
      std::ifstream conf("/etc/locale.conf");
      std::string lang;
      std::string time;
      for (std::string line; std::getline(conf, line);) {
        if (line.starts_with("LC_TIME=")) {
          time = line.substr(8);
        } else if (line.starts_with("LANG=")) {
          lang = line.substr(5);
        }
      }
      std::string region = time.empty() ? lang : time;
      std::erase(region, '"');
      return region;
    }

    // The region's first weekday: glibc first_weekday counts from week_1stday (19971130 = a Sunday).
    std::string firstWeekday() {
      locale_t loc = newlocale(LC_TIME_MASK, systemRegion().c_str(), nullptr);
      if (loc == nullptr) {
        return {};
      }
      constexpr long kSundayBase = 19971130;
      const auto week1 = reinterpret_cast<std::intptr_t>(nl_langinfo_l(_NL_TIME_WEEK_1STDAY, loc));
      const int base = week1 == kSundayBase ? 0 : 1;
      const int first = static_cast<unsigned char>(*nl_langinfo_l(_NL_TIME_FIRST_WEEKDAY, loc));
      const char* name = nl_langinfo_l(static_cast<nl_item>(DAY_1 + ((base + first - 1) % 7)), loc);
      std::string out = name != nullptr ? name : "";
      freelocale(loc);
      return out;
    }

    std::unique_ptr<Node> select(std::vector<std::string> options, std::optional<std::size_t> selected, float scale,
                                 std::function<void(std::size_t)> onChange) {
      return ui::select({
          .options = std::move(options),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Tokens::sizeControlS * scale,
          .glyphSize = Tokens::sizeSymbolInline * scale,
          .width = kSelectWidth * scale,
          .onSelectionChanged = [onChange = std::move(onChange)](std::size_t index, std::string_view) { onChange(index); },
      });
    }

    void addDateTimeCard(Flex& content, const SettingsDateTimeContext& ctx) {
      const float scale = ctx.scale;
      Flex* card = addSettingsGroupCard(content, i18n::tr("settings.datetime.title"), scale);
      if (ctx.timedate == nullptr || !ctx.timedate->ready()) {
        addInfoRow(*card, "", i18n::tr("settings.datetime.title"), formatLocal("%a %e %b %Y, %H:%M", std::time(nullptr)),
                   scale);
        addSettingsRowSeparators(*card, scale);
        addFootnote(*card, i18n::tr("settings.datetime.unavailable"), scale);
        return;
      }
      const TimeDateService& td = *ctx.timedate;
      addSwitchRow(*card, "", i18n::tr("settings.datetime.automatic"), td.ntp(), scale, ctx.setNtp);

      const auto& zones = td.timezones();
      const auto zoneIt = std::ranges::find(zones, td.timezone());
      auto zone = labelRow("", i18n::tr("settings.datetime.timezone"), scale);
      zone->addChild(select(zones, zoneIt != zones.end() ? std::optional<std::size_t>(zoneIt - zones.begin()) : std::nullopt,
                            scale, [set = ctx.setTimezone, zones](std::size_t index) { set(zones[index]); }));
      card->addChild(std::move(zone));

      const std::time_t now = td.timeUsec() > 0 ? static_cast<std::time_t>(td.timeUsec() / 1'000'000) : std::time(nullptr);
      if (td.ntp()) {
        addInfoRow(*card, "", i18n::tr("settings.datetime.title"), formatLocal("%a %e %b %Y, %H:%M", now), scale);
      } else {
        auto r = labelRow("", i18n::tr("settings.datetime.set-time"), scale);
        r->addChild(ui::input({
            .value = formatUtc(static_cast<std::int64_t>(now) * 1'000'000),
            .placeholder = std::string("YYYY-MM-DD HH:MM:SS"),
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Tokens::sizeControlS * scale,
            .width = kSelectWidth * scale,
            .onSubmit = [set = ctx.setTimeText](const std::string& text) { set(text); },
        }));
        card->addChild(std::move(r));
      }
      addSettingsRowSeparators(*card, scale);
      if (!td.lastError().empty()) {
        addFootnote(*card, td.lastError(), scale);
      }
    }

    void addFormatsCard(Flex& content, const SettingsDateTimeContext& ctx) {
      const float scale = ctx.scale;
      const ShellConfig& shell = ctx.config->shell;
      Flex* card = addSettingsGroupCard(content, i18n::tr("settings.datetime.formats"), scale);
      addSwitchRow(*card, "", i18n::tr("settings.datetime.24-hour"), is24Hour(shell.timeFormat), scale,
                   [set = ctx.setOverride, format = shell.timeFormat](bool on) {
                     set({"shell", "time_format"}, withHourCycle(format, on));
                   });

      const std::time_t now = std::time(nullptr);
      std::vector<std::string> formats(kDatePresets.begin(), kDatePresets.end());
      std::vector<std::string> labels;
      for (const auto& format : formats) {
        labels.push_back(formatLocal(format.c_str(), now));
      }
      auto current = std::ranges::find(formats, shell.dateFormat);
      if (current == formats.end()) {
        formats.push_back(shell.dateFormat);
        labels.push_back(formatLocal(shell.dateFormat.c_str(), now));
        current = formats.end() - 1;
      }
      auto r = labelRow("", i18n::tr("settings.datetime.bar-date"), scale);
      r->addChild(select(std::move(labels), static_cast<std::size_t>(current - formats.begin()), scale,
                         [set = ctx.setOverride, formats](std::size_t index) { set({"shell", "date_format"}, formats[index]); }));
      card->addChild(std::move(r));
      addSettingsRowSeparators(*card, scale);
    }

    void addCalendarCard(Flex& content, const SettingsDateTimeContext& ctx) {
      const float scale = ctx.scale;
      const Config& cfg = *ctx.config;
      Flex* card = addSettingsGroupCard(content, i18n::tr("settings.datetime.calendar"), scale);
      addInfoRow(*card, "", i18n::tr("settings.datetime.first-weekday"), firstWeekday(), scale, ctx.openRegion);
      addSwitchRow(*card, "", i18n::tr("settings.datetime.week-numbers"), cfg.controlCenter.calendarTab.showWeekNumbers,
                   scale, [set = ctx.setOverride](bool on) { set({"control_center", "calendar", "show_week_numbers"}, on); });
      addSwitchRow(*card, "", i18n::tr("settings.datetime.events"), cfg.calendar.enabled, scale,
                   [set = ctx.setOverride](bool on) { set({"calendar", "enabled"}, on); });
      if (cfg.calendar.enabled) {
        addSwitchRow(*card, "", i18n::tr("settings.datetime.reminders"), cfg.calendar.reminders.enabled, scale,
                     [set = ctx.setOverride](bool on) { set({"calendar", "reminders", "enabled"}, on); });
        for (const auto& account : ctx.accounts) {
          Flex* r = addTwoLineRow(*card, "calendar", account.name, account.status, scale);
          r->addChild(chevronButton(account.name, scale, account.open));
        }
        if (ctx.addAccount) {
          addActionRow(*card, i18n::tr("settings.datetime.add-account"), !ctx.accounts.empty(), scale, ctx.addAccount);
        }
      }
      addSettingsRowSeparators(*card, scale);
    }

  } // namespace

  void addSettingsDateTime(Flex& content, const SettingsDateTimeContext& ctx) {
    addDateTimeCard(content, ctx);
    if (ctx.config != nullptr && ctx.setOverride) {
      addFormatsCard(content, ctx);
      addCalendarCard(content, ctx);
    }
  }

} // namespace settings
