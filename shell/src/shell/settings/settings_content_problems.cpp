#include "shell/settings/settings_content_problems.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "system/crash_report.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace settings {

  namespace {

    constexpr std::size_t kMaxRows = 20;

    // Units the desktop starts: the session services it ships and the app units it launches (app-*.scope, app-*.service).
    bool isDesktopUnit(std::string_view unit) {
      return unit == "andromeda.service" || unit == "noctalia.service" || unit == "xdg-desktop-portal-andromeda.service"
          || (unit.starts_with("andromeda-") && unit.ends_with(".service")) || unit.starts_with("app-");
    }

    std::string displayName(const std::string& exe) {
      const std::string base = std::filesystem::path(exe).filename().string();
      if (base == "andromeda") {
        return i18n::tr("settings.problems.desktop");
      }
      if (base == "noctalia") {
        return i18n::tr("settings.problems.shell");
      }
      return base;
    }

    // "Today 16:43", "Yesterday 16:43", or "02 Nov 16:43".
    std::string whenText(std::int64_t timeUs) {
      const std::time_t then = static_cast<std::time_t>(timeUs / 1000000);
      const std::time_t now = std::time(nullptr);
      std::tm thenTm{};
      std::tm nowTm{};
      localtime_r(&then, &thenTm);
      localtime_r(&now, &nowTm);
      char clock[16];
      std::strftime(clock, sizeof(clock), "%H:%M", &thenTm);
      std::tm yesterday = nowTm;
      yesterday.tm_mday -= 1;
      std::mktime(&yesterday);
      if (thenTm.tm_year == nowTm.tm_year && thenTm.tm_yday == nowTm.tm_yday) {
        return i18n::tr("settings.problems.today", "time", clock);
      }
      if (thenTm.tm_year == yesterday.tm_year && thenTm.tm_yday == yesterday.tm_yday) {
        return i18n::tr("settings.problems.yesterday", "time", clock);
      }
      char date[32];
      std::strftime(date, sizeof(date), "%d %b %H:%M", &thenTm);
      return date;
    }

  } // namespace

  std::vector<CrashRecord> parseCrashRecords(std::string_view jsonLines) {
    std::vector<CrashRecord> records;
    std::size_t start = 0;
    while (start < jsonLines.size()) {
      std::size_t end = jsonLines.find('\n', start);
      if (end == std::string_view::npos) {
        end = jsonLines.size();
      }
      const nlohmann::json entry = nlohmann::json::parse(jsonLines.substr(start, end - start), nullptr, false);
      start = end + 1;
      if (!entry.is_object()) {
        continue;
      }
      const auto field = [&entry](const char* key) {
        const auto it = entry.find(key);
        return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
      };
      if (!isDesktopUnit(field("COREDUMP_USER_UNIT"))) {
        continue;
      }
      const std::string timeText = field("COREDUMP_TIMESTAMP");
      const std::string pidText = field("COREDUMP_PID");
      std::int64_t timeUs = 0;
      int pid = 0;
      std::from_chars(timeText.data(), timeText.data() + timeText.size(), timeUs);
      std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
      records.push_back({.exe = field("COREDUMP_EXE"), .pid = pid, .timeUs = timeUs});
    }
    std::ranges::sort(records, std::ranges::greater{}, &CrashRecord::timeUs);
    if (records.size() > kMaxRows) {
      records.resize(kMaxRows);
    }
    return records;
  }

  void addSettingsProblems(
      Flex& content, float scale, const std::optional<std::vector<CrashRecord>>& records, bool available
  ) {
    if (!records.has_value()) {
      return;
    }
    if (!available || records->empty()) {
      content.addChild(makeLabel(
          i18n::tr(available ? "settings.problems.empty" : "settings.problems.unavailable"),
          Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label2)
      ));
    } else {
      auto list = ui::column(
          {.gap = Style::spaceXs * scale,
           .paddingV = Style::spaceSm * scale,
           .paddingH = Style::spaceMd * scale,
           .fill = colorSpecFromRole(ColorRole::SolidRegular),
           .radius = Style::scaledRadiusMd(scale),
           .fillWidth = true}
      );
      for (const CrashRecord& record : *records) {
        const int pid = record.pid;
        list->addChild(ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceMd * scale,
             .minHeight = Style::controlHeightLg * scale,
             .fillWidth = true},
            ui::glyph({
                .glyph = "alert-triangle",
                .glyphSize = Style::fontSizeTitle * scale,
                .color = colorSpecFromRole(ColorRole::Label2),
            }),
            ui::column(
                {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
                makeLabel(
                    displayName(record.exe), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1),
                    FontWeight::Bold
                ),
                makeLabel(
                    i18n::tr("settings.problems.stopped", "when", whenText(record.timeUs)),
                    Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)
                )
            ),
            ui::button({
                .text = i18n::tr("settings.problems.save-report"),
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .variant = ButtonVariant::Default,
                .onClick = [pid]() { crash_report::save(pid); },
            })
        ));
      }
      content.addChild(std::move(list));
    }
    content.addChild(ui::row(
        {.align = FlexAlign::Center, .fillWidth = true},
        ui::button({
            .text = i18n::tr("settings.problems.open-folder"),
            .glyph = "folder",
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .variant = ButtonVariant::Default,
            .onClick = []() { crash_report::openDirectory(); },
        })
    ));
  }

} // namespace settings
