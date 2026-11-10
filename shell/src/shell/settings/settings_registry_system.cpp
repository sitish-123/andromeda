#include "shell/settings/settings_registry_internal.h"
#include "config/app_icon_settings.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/system_interactive_call.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "pipewire/sound_player.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "ui/app_icon_colorization.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  using namespace registry_detail;

  void registry_detail::add_system_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // System
    if (env.batteryAvailable) {
      if (env.systemBatteryAvailable) {
        entries.push_back(makeEntry(
            SettingsSection::System, "battery", tr("settings.schema.system.battery-warning-threshold.label"),
            tr("settings.schema.system.battery-warning-threshold.description"), {"battery", "warning_threshold"},
            sliderFor(cfg.battery.warningThreshold, noctalia::config::schema::kBatteryWarningThresholdRange, true),
            "battery low warning threshold notification"
        ));
        // Values are power-profiles-daemon profile names; empty keeps the current profile.
        const auto profileOptions = [&] {
          return std::vector<SelectOption>{
              {.value = "", .label = tr("settings.schema.system.battery-profile.keep")},
              {.value = "power-saver", .label = tr("power.profiles.power-saver")},
              {.value = "balanced", .label = tr("power.profiles.balanced")},
              {.value = "performance", .label = tr("power.profiles.performance")},
          };
        };
        entries.push_back(makeEntry(
            SettingsSection::System, "battery", tr("settings.schema.system.battery-profile-on-ac.label"),
            tr("settings.schema.system.battery-profile-on-ac.description"), {"battery", "profile_on_ac"},
            SelectSetting{profileOptions(), cfg.battery.profileOnAc, true}, "power mode profile charger plugged ac"
        ));
        entries.push_back(makeEntry(
            SettingsSection::System, "battery", tr("settings.schema.system.battery-profile-on-battery.label"),
            tr("settings.schema.system.battery-profile-on-battery.description"), {"battery", "profile_on_battery"},
            SelectSetting{profileOptions(), cfg.battery.profileOnBattery, true}, "power mode profile battery unplugged"
        ));
      }
      for (const auto& device : env.batteryDeviceOptions) {
        int value = 0;
        if (const auto it = env.batteryWarningThresholds.find(device.value); it != env.batteryWarningThresholds.end()) {
          value = it->second;
        }
        entries.push_back(makeEntry(
            SettingsSection::System, "battery",
            tr("settings.schema.system.battery-device-warning-threshold.label", "device", device.label),
            tr("settings.schema.system.battery-device-warning-threshold.description"),
            {"battery", "device", device.value, "warning_threshold"},
            SliderSetting{std::clamp(value, 0, 100), 0.0F, 100.0F, 1.0F, true},
            std::string("battery device low warning threshold notification ") + device.label + " " + device.value
        ));
      }
    }
    if (!env.schedExtScheduler.empty()) {
      entries.push_back(makeEntry(
          SettingsSection::System, "performance", tr("settings.schema.system.app-priority.label"),
          tr("settings.schema.system.app-priority.description", "scheduler", env.schedExtScheduler), {},
          ButtonSetting{
              .label = tr("settings.schema.system.app-priority.button"),
              .action = [] { (void)process::runAsync("xdg-open https://docs.kernel.org/scheduler/sched-ext.html"); },
          },
          "app priority cpu scheduler sched_ext scx boost performance"
      ));
    }
    if (env.openActivity) {
      const std::size_t count = env.backgroundActivityCount.value_or(0);
      entries.push_back(makeEntry(
          SettingsSection::System, "performance", tr("settings.schema.system.background-activity.label"),
          !env.backgroundActivityCount ? tr("settings.schema.system.background-activity.description-unchecked")
          : count == 0                 ? tr("settings.schema.system.background-activity.description-none")
          : count == 1 ? tr("settings.schema.system.background-activity.description-one")
                       : tr("settings.schema.system.background-activity.description", "count", std::to_string(count)),
          {}, ButtonSetting{.label = tr("settings.schema.system.background-activity.button"), .action = env.openActivity},
          "background activity services task manager processes memory cpu autostart login"
      ));
    }
    appendRow(entries, shellRows, "screen_time_enabled");
    const SettingVisibility monitorOn = [](const Config& c) { return c.system.monitor.enabled; };
    auto systemMonitorRows = generateSchemaEntries(
        noctalia::config::schema::systemMonitorRowsSchema(), cfg.system.monitor, {"system", "monitor"}
    );
    appendRow(entries, systemMonitorRows, "enabled");
    {
      // The slider goes down to 0, which disables the metric (no polling, no dGPU wakeups).
      constexpr float kPollMin = SystemConfig::MonitorConfig::kDisabledPollSeconds;
      constexpr float kPollMax = SystemConfig::MonitorConfig::kMaxPollSeconds;
      constexpr float kPollStep = 1.0F;
      const auto& mon = cfg.system.monitor;
      auto addPoll = [&](std::string_view labelKey, std::string_view descKey, std::vector<std::string> path,
                         float value) {
        const float clampedValue = std::clamp(value, kPollMin, kPollMax);
        SliderSetting slider{clampedValue, kPollMin, kPollMax, kPollStep, true};
        slider.valueSuffix = "s";
        auto entry = makeEntry(
            SettingsSection::System, "monitor-polling", tr(labelKey), tr(descKey), std::move(path), std::move(slider),
            "system monitor", true
        );
        entry.visibleWhen = monitorOn;
        entries.push_back(std::move(entry));
      };
      addPoll(
          "settings.schema.services.system-monitor.cpu-poll.label",
          "settings.schema.services.system-monitor.cpu-poll.description", {"system", "monitor", "cpu_poll_seconds"},
          mon.cpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.gpu-poll.label",
          "settings.schema.services.system-monitor.gpu-poll.description", {"system", "monitor", "gpu_poll_seconds"},
          mon.gpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.memory-poll.label",
          "settings.schema.services.system-monitor.memory-poll.description",
          {"system", "monitor", "memory_poll_seconds"}, mon.memoryPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.network-poll.label",
          "settings.schema.services.system-monitor.network-poll.description",
          {"system", "monitor", "network_poll_seconds"}, mon.networkPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.disk-poll.label",
          "settings.schema.services.system-monitor.disk-poll.description", {"system", "monitor", "disk_poll_seconds"},
          mon.diskPollSeconds
      );

      // One dual-thumb range row per metric: low thumb = activity threshold, high thumb = critical.
      auto addThresholdPair = [&](std::string_view baseKey, std::string_view statLabelKey, double activityValue,
                                  double criticalValue, noctalia::sysmon::ThresholdProfile profile, bool integerValue,
                                  std::string valueSuffix) {
        const std::vector<std::string> activityPath = {
            "system", "monitor", std::string(baseKey) + "_activity_threshold"
        };
        const std::vector<std::string> criticalPath = {
            "system", "monitor", std::string(baseKey) + "_critical_threshold"
        };
        const std::string statLabel = tr(statLabelKey);

        RangeSliderSetting range;
        range.lowValue = activityValue;
        range.highValue = criticalValue;
        range.minValue = profile.minValue;
        range.maxValue = profile.maxValue;
        range.step = profile.step;
        range.integerValue = integerValue;
        range.valueSuffix = std::move(valueSuffix);
        range.highPath = criticalPath;

        auto entry = makeEntry(
            SettingsSection::System, "monitor-thresholds",
            tr("settings.schema.services.system-monitor.threshold.label", "stat", statLabel),
            tr("settings.schema.services.system-monitor.threshold.description"), activityPath, std::move(range),
            "system monitor threshold activity critical", true
        );
        entry.visibleWhen = monitorOn;
        entries.push_back(std::move(entry));
      };

      using noctalia::sysmon::Stat;
      addThresholdPair(
          "cpu_usage", "settings.schema.services.system-monitor.stats.cpu-usage", mon.cpuUsageActivityThreshold,
          mon.cpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuUsage), true, "%"
      );
      addThresholdPair(
          "cpu_temp", "settings.schema.services.system-monitor.stats.cpu-temp", mon.cpuTempActivityThreshold,
          mon.cpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuTemp), true, "°C"
      );
      addThresholdPair(
          "cpu_freq", "settings.schema.services.system-monitor.stats.cpu-freq", mon.cpuFreqActivityThreshold,
          mon.cpuFreqCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuFreq), false, "GHz"
      );
      addThresholdPair(
          "gpu_usage", "settings.schema.services.system-monitor.stats.gpu-usage", mon.gpuUsageActivityThreshold,
          mon.gpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuUsage), true, "%"
      );
      addThresholdPair(
          "gpu_temp", "settings.schema.services.system-monitor.stats.gpu-temp", mon.gpuTempActivityThreshold,
          mon.gpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuTemp), true, "°C"
      );
      addThresholdPair(
          "gpu_vram", "settings.schema.services.system-monitor.stats.gpu-vram", mon.gpuVramActivityThreshold,
          mon.gpuVramCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuVram), true, "%"
      );
      addThresholdPair(
          "ram_pct", "settings.schema.services.system-monitor.stats.ram-usage", mon.ramPctActivityThreshold,
          mon.ramPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::RamPct), true, "%"
      );
      addThresholdPair(
          "swap_pct", "settings.schema.services.system-monitor.stats.swap-usage", mon.swapPctActivityThreshold,
          mon.swapPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::SwapPct), true, "%"
      );
      addThresholdPair(
          "disk_used_pct", "settings.schema.services.system-monitor.stats.disk-used-pct",
          mon.diskUsedPctActivityThreshold, mon.diskUsedPctCriticalThreshold,
          noctalia::sysmon::thresholdProfile(Stat::DiskUsedPct), true, "%"
      );
      addThresholdPair(
          "disk_used", "settings.schema.services.system-monitor.stats.disk-used", mon.diskUsedActivityThreshold,
          mon.diskUsedCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::DiskUsed), true, "%"
      );
      addThresholdPair(
          "disk_free_pct", "settings.schema.services.system-monitor.stats.disk-free-pct",
          mon.diskFreePctActivityThreshold, mon.diskFreePctCriticalThreshold,
          noctalia::sysmon::thresholdProfile(Stat::DiskFreePct), true, "%"
      );
      addThresholdPair(
          "disk_free", "settings.schema.services.system-monitor.stats.disk-free", mon.diskFreeActivityThreshold,
          mon.diskFreeCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::DiskFree), true, "%"
      );
      addThresholdPair(
          "net_rx", "settings.schema.services.system-monitor.stats.network-rx", mon.netRxActivityThreshold,
          mon.netRxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetRx), false, "MB/s"
      );
      addThresholdPair(
          "net_tx", "settings.schema.services.system-monitor.stats.network-tx", mon.netTxActivityThreshold,
          mon.netTxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetTx), false, "MB/s"
      );
    }

    // Location: single source of "where am I"; shared by weather, night light, and theme auto mode.
    auto locationRows = generateSchemaEntries(noctalia::config::schema::locationSchema(), cfg.location, {"location"});
    appendRow(entries, locationRows, "auto_locate");
    const SettingVisibility autoLocateOff = [](const Config& c) { return !c.location.autoLocate; };
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.location-address.label"),
          tr("settings.schema.services.location-address.description"), {"location", "address"},
          TextSetting{
              .value = cfg.location.address,
              .placeholder = tr("settings.schema.services.location-address.placeholder"),
              .browseFileExtensions = {}
          },
          "location address city geocode"
      );
      e.visibleWhen = autoLocateOff;
      entries.push_back(std::move(e));
    }
    // Manual coordinates: shown only when no network location is configured (auto-locate off
    // and no address). The address gate is build-time; the auto-locate gate is evaluated live.
    const SettingVisibility manualLocationHidden = [](const Config&) { return false; };
    const SettingVisibility& manualLocationControlsVisible =
        cfg.location.address.empty() ? autoLocateOff : manualLocationHidden;
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.latitude.label"),
          tr("settings.schema.services.latitude.description"), {"location", "latitude"},
          OptionalNumberSetting{cfg.location.latitude, -90.0, 90.0, "52.5200"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.longitude.label"),
          tr("settings.schema.services.longitude.description"), {"location", "longitude"},
          OptionalNumberSetting{cfg.location.longitude, -180.0, 180.0, "13.4050"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }

    // Custom scheduling: explicit sunrise/sunset times for night light and theme auto mode.
    {
      auto e = makeEntry(
          SettingsSection::Location, "location", tr("settings.schema.services.custom-schedule.label"),
          tr("settings.schema.services.custom-schedule.description"), {"location", "custom_schedule"},
          ToggleSetting{cfg.location.customSchedule}, "schedule custom time sunrise sunset"
      );
      entries.push_back(std::move(e));
    }
    // Keep both required times editable before activation so the schedule can be valid when enabled.
    appendRow(entries, locationRows, "sunset");
    appendRow(entries, locationRows, "sunrise");

    // Weather: consumes the resolved location.
    auto weatherRows = generateSchemaEntries(noctalia::config::schema::weatherSchema(), cfg.weather, {"weather"});
    appendRow(entries, weatherRows, "enabled");
    const SettingVisibility weatherOn = [](const Config& c) { return c.weather.enabled; };
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.shell.show-location.label"),
          tr("settings.schema.shell.show-location.description"), {"shell", "show_location"},
          ToggleSetting{cfg.shell.showLocation}, "weather"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-unit.label"),
          tr("settings.schema.services.weather-unit.description"), {"weather", "unit"},
          asSegmented(plainSelect(
              {{"metric", "settings.options.weather.unit.metric"},
               {"imperial", "settings.options.weather.unit.imperial"}},
              cfg.weather.unit
          )),
          "temperature"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-effects.label"),
          tr("settings.schema.services.weather-effects.description"), {"weather", "effects"},
          ToggleSetting{cfg.weather.effects}, "forecast visuals"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Location, "weather", tr("settings.schema.services.weather-refresh-interval.label"),
          tr("settings.schema.services.weather-refresh-interval.description"), {"weather", "refresh_minutes"},
          sliderFor(cfg.weather.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "forecast"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }

    if (!env.gammaControlAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::Location, "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.requires-gamma-control"), {"nightlight", "enabled"},
          ToggleSetting{.checked = cfg.nightlight.enabled, .enabled = false}, "nightlight"
      ));
    } else {
      entries.push_back(makeEntry(
          SettingsSection::Location, "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.description"), {"nightlight", "enabled"},
          ToggleSetting{cfg.nightlight.enabled}, "nightlight"
      ));
      const SettingVisibility nightLightOn = [](const Config& c) { return c.nightlight.enabled; };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.force-night-light.label"),
            tr("settings.schema.services.force-night-light.description"), {"nightlight", "force"},
            ToggleSetting{cfg.nightlight.force}, "nightlight"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
      // Both sliders span the same range; the day > night invariant is enforced at commit time
      // via SliderSetting::linkedCommit, which pushes the other temperature when needed.
      const auto tempMin = static_cast<double>(NightLightConfig::kTemperatureMin);
      const auto tempMax = static_cast<double>(NightLightConfig::kTemperatureMax);
      const auto tempStep = static_cast<double>(NightLightConfig::kTemperatureGap);

      SliderSetting daySlider{static_cast<double>(cfg.nightlight.dayTemperature), tempMin, tempMax, tempStep, true};
      daySlider.linkedCommit = [curNight = cfg.nightlight.nightTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newDay = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (newDay - NightLightConfig::kTemperatureGap < curNight) {
          std::int32_t pushedNight =
              std::max(NightLightConfig::kTemperatureMin, newDay - NightLightConfig::kTemperatureGap);
          if (pushedNight + NightLightConfig::kTemperatureGap > newDay) {
            // Day was below kTemperatureMin + kTemperatureGap; bump day up too. The slider value
            // refresh comes through the rebuilt registry on the next config reload.
            const std::int32_t bumpedDay =
                std::min(NightLightConfig::kTemperatureMax, pushedNight + NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(bumpedDay)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(pushedNight)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.day-temperature.label"),
            tr("settings.schema.services.day-temperature.description"), {"nightlight", "temperature_day"},
            std::move(daySlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }

      SliderSetting nightSlider{static_cast<double>(cfg.nightlight.nightTemperature), tempMin, tempMax, tempStep, true};
      nightSlider.linkedCommit = [curDay = cfg.nightlight.dayTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newNight = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (curDay - NightLightConfig::kTemperatureGap < newNight) {
          std::int32_t pushedDay =
              std::min(NightLightConfig::kTemperatureMax, newNight + NightLightConfig::kTemperatureGap);
          if (pushedDay - NightLightConfig::kTemperatureGap < newNight) {
            const std::int32_t bumpedNight =
                std::max(NightLightConfig::kTemperatureMin, pushedDay - NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(bumpedNight)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(pushedDay)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            SettingsSection::Location, "night-light", tr("settings.schema.services.night-temperature.label"),
            tr("settings.schema.services.night-temperature.description"), {"nightlight", "temperature_night"},
            std::move(nightSlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
    }

    const SettingVisibility calendarOn = [](const Config& c) { return c.calendar.enabled; };
    auto calendarRows = generateSchemaEntries(noctalia::config::schema::calendarSchema(), cfg.calendar, {"calendar"});
    appendRow(entries, calendarRows, "enabled");
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-date-format.label"),
          tr("settings.schema.services.calendar-event-date-format.description"), {"calendar", "event_date_format"},
          TextSetting{.value = cfg.calendar.eventDateFormat, .placeholder = "%A %e %B", .browseFileExtensions = {}},
          "calendar date format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-time-format.label"),
          tr("settings.schema.services.calendar-event-time-format.description"), {"calendar", "event_time_format"},
          TextSetting{.value = cfg.calendar.eventTimeFormat, .placeholder = "%H:%M", .browseFileExtensions = {}},
          "calendar time format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-events.label"),
          tr("settings.schema.services.calendar-dedupe-events.description"), {"calendar", "dedupe_events"},
          ToggleSetting{cfg.calendar.dedupeEvents}, "calendar events duplicate merge"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-ignore-patterns.label"),
          tr("settings.schema.services.calendar-dedupe-ignore-patterns.description"),
          {"calendar", "dedupe_ignore_patterns"}, ListSetting{.items = cfg.calendar.dedupeIgnorePatterns},
          "calendar events duplicate merge regex title"
      );
      e.visibleWhen = [](const Config& c) { return c.calendar.enabled && c.calendar.dedupeEvents; };
      entries.push_back(std::move(e));
    }
    // Week numbers are a grid decoration, so they stay available when event syncing is off.
    auto calendarTabRows = generateSchemaEntries(
        noctalia::config::schema::calendarTabRowsSchema(), cfg.controlCenter.calendarTab, {"control_center", "calendar"}
    );
    appendGroup(entries, calendarTabRows, "general");
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-past-events-threshold.label"),
          tr("settings.schema.services.calendar-past-events-threshold.description"),
          {"control_center", "calendar", "past_events_threshold_minutes"},
          sliderFor(
              cfg.controlCenter.calendarTab.pastEventsThresholdMinutes,
              noctalia::config::schema::kPastEventsThresholdMinutesRange, true
          ),
          "calendar past events threshold minutes hide old finished"
      );
      e.visibleWhen = [](const Config& c) { return c.controlCenter.calendarTab.hidePastEvents; };
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminders.label"),
          tr("settings.schema.services.calendar-reminders.description"), {"calendar", "reminders", "enabled"},
          ToggleSetting{cfg.calendar.reminders.enabled}, "calendar reminder notification alarm upcoming"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    const SettingVisibility remindersOn = [](const Config& c) {
      return c.calendar.enabled && c.calendar.reminders.enabled;
    };
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders",
          tr("settings.schema.services.calendar-reminder-use-event.label"),
          tr("settings.schema.services.calendar-reminder-use-event.description"),
          {"calendar", "reminders", "use_event_reminders"}, ToggleSetting{cfg.calendar.reminders.useEventReminders},
          "calendar reminder valarm override event"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminder-lead.label"),
          tr("settings.schema.services.calendar-reminder-lead.description"),
          {"calendar", "reminders", "default_lead_minutes"},
          sliderFor(
              cfg.calendar.reminders.defaultLeadMinutes, noctalia::config::schema::kReminderLeadMinutesRange, true
          ),
          "calendar reminder lead minutes before default"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-digest-time.label"),
          tr("settings.schema.services.calendar-digest-time.description"),
          {"calendar", "reminders", "all_day_digest_time"},
          TextSetting{
              .value = cfg.calendar.reminders.allDayDigestTime, .placeholder = "09:00", .browseFileExtensions = {}
          },
          "calendar all day digest time morning"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    // Sync cadence belongs with the accounts it drives; the account rows are injected right after it.
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-accounts",
          tr("settings.schema.services.calendar-refresh-interval.label"),
          tr("settings.schema.services.calendar-refresh-interval.description"), {"calendar", "refresh_minutes"},
          sliderFor(cfg.calendar.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "calendar sync"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      // timedated keeps the RTC mode in /etc/adjtime (third line LOCAL or UTC).
      std::ifstream adjtime("/etc/adjtime");
      std::string line;
      for (int i = 0; i < 3 && std::getline(adjtime, line); ++i) {
      }
      auto e = makeEntry(
          SettingsSection::DateTime, "clock", tr("settings.datetime.local-rtc"),
          tr("settings.datetime.local-rtc-description"), {},
          ToggleSetting{
              .checked = line == "LOCAL",
              .id = "datetime.local_rtc",
              .onChange =
                  [](bool on, std::function<void()> done) {
                    dbus::callInteractive(
                        "org.freedesktop.timedate1", "/org/freedesktop/timedate1", "org.freedesktop.timedate1",
                        "SetLocalRTC", g_variant_new("(bbb)", on, TRUE, TRUE),
                        [done = std::move(done)](GVariant*, std::string) { done(); }
                    );
                  },
          },
          "rtc hardware clock local time utc dual boot windows"
      );
      e.advanced = true;
      entries.push_back(std::move(e));
    }

    auto audioRows = generateSchemaEntries(noctalia::config::schema::audioSchema(), cfg.audio, {"audio"});
    for (const auto* key : {"enable_overdrive", "auto_switch_hdmi", "enable_sounds"}) {
      appendRow(entries, audioRows, key);
    }
    const std::size_t soundsGatedStart = entries.size();
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.volume-sounds.label"),
        tr("settings.schema.services.volume-sounds.description"), {"audio", "enable_volume_sounds"},
        ToggleSetting{cfg.audio.enableVolumeSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.notification-sounds.label"),
        tr("settings.schema.services.notification-sounds.description"), {"audio", "enable_notification_sounds"},
        ToggleSetting{cfg.audio.enableNotificationSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.power-sounds.label"),
        tr("settings.schema.services.power-sounds.description"), {"audio", "enable_power_sounds"},
        ToggleSetting{cfg.audio.enablePowerSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.screenshot-sounds.label"),
        tr("settings.schema.services.screenshot-sounds.description"), {"audio", "enable_screenshot_sounds"},
        ToggleSetting{cfg.audio.enableScreenshotSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-volume.label"),
        tr("settings.schema.services.sound-volume.description"), {"audio", "sound_volume"},
        sliderFor(cfg.audio.soundVolume, noctalia::config::schema::kUnitRange, false), "sound"
    ));
    std::vector<SelectOption> soundThemeOptions;
    for (const auto& [value, label] : SoundPlayer::availableThemes()) {
      soundThemeOptions.push_back(SelectOption{value, label});
    }
    if (soundThemeOptions.empty()) {
      soundThemeOptions.push_back(SelectOption{"freedesktop", tr("settings.schema.services.sound-theme.default")});
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-theme.label"),
        tr("settings.schema.services.sound-theme.description"), {"audio", "sound_theme"},
        SelectSetting{std::move(soundThemeOptions), cfg.audio.soundTheme}, "sound theme"
    ));
    for (std::size_t i = soundsGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.audio.enableSounds && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.audio.enableSounds; }};
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "brightness", tr("settings.schema.services.ddcutil.label"),
        env.ddcutilAvailable ? tr("settings.schema.services.ddcutil.description")
                             : tr("settings.schema.services.ddcutil.requires-ddcutil"),
        {"brightness", "enable_ddcutil"},
        ToggleSetting{.checked = cfg.brightness.enableDdcutil, .enabled = env.ddcutilAvailable}, "monitor ddcutil"
    ));
    auto brightnessRows =
        generateSchemaEntries(noctalia::config::schema::brightnessSchema(), cfg.brightness, {"brightness"});
    appendRow(entries, brightnessRows, "minimum_brightness");
    appendRow(entries, brightnessRows, "sync_all_monitors");
    appendRow(entries, brightnessRows, "automatic");
    auto bluetoothRows =
        generateSchemaEntries(noctalia::config::schema::bluetoothSchema(), cfg.bluetooth, {"bluetooth"});
    appendGroup(entries, bluetoothRows, "bluetooth");
    entries.push_back(makeEntry(
        SettingsSection::Services, "media", tr("settings.schema.services.mpris-blacklist.label"),
        tr("settings.schema.services.mpris-blacklist.description"), {"shell", "mpris", "blacklist"},
        ListSetting{.items = cfg.shell.mpris.blacklist}, "mpris media player dbus session blacklist"
    ));
  }

  void registry_detail::add_power_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;

    // Power
    auto sessionRows =
        generateSchemaEntries(noctalia::config::schema::sessionSchema(), cfg.shell.session, {"shell", "session"});
    appendRow(entries, sessionRows, "show_shortcuts");
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.power.session-actions.label"),
        tr("settings.schema.power.session-actions.description"), {"shell", "session", "actions"},
        SessionPanelActionsSetting{.items = cfg.shell.session.actions},
        "session panel power menu logout reboot shutdown lock command actions order"
    ));

    // Idle
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.pre-action-fade.label"),
        tr("settings.schema.idle.pre-action-fade.description"), {"idle", "pre_action_fade_seconds"},
        StepperSetting{
            .value = static_cast<int>(std::lround(std::clamp(cfg.idle.preActionFadeSeconds, 0.0F, 30.0F))),
            .minValue = 0,
            .maxValue = 30,
            .step = 1,
            .valueSuffix = "s"
        },
        "idle fade dim seconds overlay"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.behaviors.label"),
        tr("settings.schema.idle.behaviors.description"), {"idle", "behavior"},
        IdleBehaviorsSetting{.items = cfg.idle.behaviors},
        "idle behavior timeout command resume screen lock dpms suspend lock_and_suspend hibernate caffeine"
    ));

    // Hooks
    auto hookGroup = [](HookKind kind) -> std::string {
      switch (kind) {
      case HookKind::Started:
      case HookKind::SessionLocked:
      case HookKind::SessionUnlocked:
      case HookKind::LoggingOut:
      case HookKind::Rebooting:
      case HookKind::ShuttingDown:
        return "lifecycle";
      case HookKind::WallpaperChanged:
      case HookKind::ColorsChanged:
      case HookKind::ThemeModeChanged:
        return "theme";
      case HookKind::WifiEnabled:
      case HookKind::WifiDisabled:
      case HookKind::BluetoothEnabled:
      case HookKind::BluetoothDisabled:
        return "network";
      case HookKind::BatteryCharging:
      case HookKind::BatteryDischarging:
      case HookKind::BatteryPlugged:
      case HookKind::BatteryPercentageChanged:
      case HookKind::PowerProfileChanged:
        return "power";
      case HookKind::Count:
        break;
      }
      return "general";
    };

    auto hookTags = [](HookKind kind) -> std::string {
      std::string tags = "hook command script exec event trigger";
      if (kind == HookKind::BatteryCharging
          || kind == HookKind::BatteryDischarging
          || kind == HookKind::BatteryPlugged
          || kind == HookKind::BatteryPercentageChanged) {
        tags += " battery power";
      }
      if (kind == HookKind::PowerProfileChanged) {
        tags += " power profile performance balanced saver";
      }
      if (kind == HookKind::WallpaperChanged || kind == HookKind::ColorsChanged || kind == HookKind::ThemeModeChanged) {
        tags += " wallpaper colors theme mode light dark auto";
      }
      if (kind == HookKind::WifiEnabled
          || kind == HookKind::WifiDisabled
          || kind == HookKind::BluetoothEnabled
          || kind == HookKind::BluetoothDisabled) {
        tags += " network wifi bluetooth";
      }
      if (kind == HookKind::SessionLocked
          || kind == HookKind::SessionUnlocked
          || kind == HookKind::LoggingOut
          || kind == HookKind::Rebooting
          || kind == HookKind::ShuttingDown
          || kind == HookKind::Started) {
        tags += " session startup";
      }
      return tags;
    };

    for (const auto& kind : kHookKinds) {
      const auto index = static_cast<std::size_t>(kind.value);
      const std::string key(kind.key);
      const std::string baseKey = "settings.schema.hooks.events." + i18n::keySegment(key);
      const std::string hookCmd = cfg.hooks.commands[index].empty() ? "" : cfg.hooks.commands[index][0];
      entries.push_back(makeEntry(
          SettingsSection::Hooks, hookGroup(kind.value), tr(baseKey + ".label"), tr(baseKey + ".description"),
          {"hooks", key},
          TextSetting{
              .value = hookCmd,
              .placeholder = tr("settings.schema.hooks.command-placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          hookTags(kind.value)
      ));
    }
  }

  void registry_detail::add_notifications_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    auto& entries = ctx.entries;

    // Notifications
    auto notificationRows =
        generateSchemaEntries(noctalia::config::schema::notificationSchema(), cfg.notification, {"notification"});
    appendGroup(entries, notificationRows, "general");
    appendGroup(entries, notificationRows, "history");
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "filtering", tr("settings.schema.notifications.filters.label"),
        tr("settings.schema.notifications.filters.description"), {"notification", "filter"},
        NotificationFiltersSetting{.items = cfg.notification.filters},
        "filter blacklist suppress toast history sound dnd bypass do not disturb app name desktop entry category "
        "urgency"
    ));
  }

} // namespace settings
