#include "shell/settings/settings_content_problems.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "system/crash_report.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <ctime>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace settings {

  namespace {

    constexpr std::size_t kMaxRows = 20;

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

  std::vector<CrashRecord> parseCrashRecords(std::string_view json) {
    std::vector<CrashRecord> records;
    const nlohmann::json list = nlohmann::json::parse(json, nullptr, false);
    if (!list.is_array()) {
      return records;
    }
    for (const auto& entry : list) {
      if (entry.is_object()) {
        records.push_back(
            {.exe = entry.value("exe", ""), .pid = entry.value("pid", 0), .timeUs = entry.value("time", std::int64_t{0})}
        );
      }
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
