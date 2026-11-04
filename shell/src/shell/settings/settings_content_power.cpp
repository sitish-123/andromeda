#include "shell/settings/settings_content_power.h"

#include "core/process/process.h"
#include "dbus/andromeda/system_health_service.h"
#include "dbus/logind/power_config_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_selection_style.h"
#include "system/activity_monitor.h"
#include "system/battery_history.h"
#include "system/battery_impact.h"
#include "system/screen_on_time.h"
#include "system/screen_time_service.h"
#include "protocols/app_energy_log.h"
#include "protocols/screen_activity_log.h"
#include "ui/controls/graph.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    using namespace page_rows;

    std::string batteryStatus(const UPowerState& state) {
      const bool charging = state.state == BatteryState::Charging || state.state == BatteryState::PendingCharge;
      const std::int64_t seconds = charging ? state.timeToFull : state.timeToEmpty;
      std::string status = batteryStateLabel(state.state);
      if (seconds > 0) {
        status += " · " + i18n::tr(charging ? "control-center.power.time-to-full" : "control-center.power.time-to-empty",
                                   "time", formatDuration(std::chrono::seconds{seconds}));
      }
      return status;
    }

    constexpr auto kLowPowerRule = "power.low_power_plan";

    std::string ruleLabel(const std::string& id) {
      if (id == kLowPowerRule) {
        return i18n::tr("settings.power.rule-low-power");
      }
      if (id == "battery.critical") {
        return i18n::tr("settings.power.rule-battery-critical");
      }
      if (id == "apps.drainer") {
        return i18n::tr("settings.power.rule-drainer");
      }
      if (id == "memory.relief") {
        return i18n::tr("settings.power.rule-memory");
      }
      if (id == "thermal.relief") {
        return i18n::tr("settings.power.rule-thermal");
      }
      if (id == "storage.low") {
        return i18n::tr("settings.power.rule-storage");
      }
      if (id == "thermal.hot") {
        return i18n::tr("settings.power.rule-thermal-hot");
      }
      if (id == "battery.optimized_charging") {
        return i18n::tr("settings.power.rule-optimized-charging");
      }
      return i18n::tr("settings.power.rule-other");
    }

    std::unique_ptr<Flex> twoLines(std::string text, std::string sub, float scale) {
      auto lines = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
      lines->addChild(ui::label({
          .text = std::move(text), .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
      }));
      lines->addChild(ui::label({
          .text = std::move(sub), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2),
          .maxLines = 1,
      }));
      return lines;
    }

    // Bytes of the first UTF-8 character of a non-empty string.
    std::size_t firstCharBytes(std::string_view text) {
      const auto lead = static_cast<unsigned char>(text.front());
      const std::size_t bytes = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
      return std::min(bytes, text.size());
    }

    // Energy saving card (mockup A): power mode, the rule that limits it, and the low power threshold.
    void addEnergySaving(Flex& content, PowerProfilesService& profiles, SystemHealthService* health, float scale) {
      std::vector<std::string> shown;
      for (const std::string_view id : powerProfileOrder()) {
        if (std::ranges::contains(profiles.profiles(), id)) {
          shown.emplace_back(id);
        }
      }
      if (shown.empty()) {
        return;
      }
      std::vector<ui::SegmentedOption> options;
      std::size_t selected = 0;
      for (std::size_t i = 0; i < shown.size(); ++i) {
        options.push_back(ui::SegmentedOption{.label = profileLabel(shown[i])});
        if (shown[i] == profiles.activeProfile()) {
          selected = i;
        }
      }
      Flex* card = addCard(content, scale);
      auto r = row(scale);
      const std::string limitedBy = health != nullptr ? health->limitedBy("power.profile") : std::string{};
      if (limitedBy.empty()) {
        r->addChild(title(i18n::tr("settings.power.mode"), scale));
      } else {
        r->addChild(twoLines(
            i18n::tr("settings.power.mode"), i18n::tr("settings.power.limited-by", "rule", ruleLabel(limitedBy)), scale
        ));
      }
      r->addChild(ui::segmented({
          .options = std::move(options),
          .selectedIndex = selected,
          .scale = scale,
          .compact = true,
          .onChange = [&profiles, shown](std::size_t index) { (void)profiles.setActiveProfile(shown[index]); },
      }));
      card->addChild(std::move(r));

      const SystemRule* rule = health != nullptr ? health->rule(kLowPowerRule) : nullptr;
      if (rule == nullptr || !rule->error.empty()) {
        return;
      }
      static constexpr int kThresholds[] = {10, 15, 20, 25, 30};
      std::vector<std::string> labels{i18n::tr("settings.power.low-power-off")};
      std::size_t current = 0;
      const auto pct = rule->params.find("low_pct");
      for (std::size_t i = 0; i < std::size(kThresholds); ++i) {
        labels.push_back(std::format("{} %", kThresholds[i]));
        if (rule->enabled && pct != rule->params.end() && std::lround(pct->second) == kThresholds[i]) {
          current = i + 1;
        }
      }
      auto t = row(scale);
      t->addChild(title(i18n::tr("settings.power.low-power-below"), scale));
      t->addChild(ui::select({
          .options = std::move(labels),
          .selectedIndex = current,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = 96.0F * scale,
          .onSelectionChanged =
              [health, current](std::size_t index, std::string_view /*label*/) {
                if (index == current) {
                  return;
                }
                if (index == 0) {
                  health->setRule(kLowPowerRule, false, {});
                } else {
                  health->setRule(kLowPowerRule, true, {{"low_pct", static_cast<double>(kThresholds[index - 1])}});
                }
              },
      }));
      card->addChild(std::move(t));
    }

    std::string entryTime(std::uint64_t usec) {
      const std::time_t when = static_cast<std::time_t>(usec / 1000000);
      const std::time_t now = std::time(nullptr);
      std::tm at{};
      std::tm today{};
      localtime_r(&when, &at);
      localtime_r(&now, &today);
      const std::string clock = std::format("{:02}:{:02}", at.tm_hour, at.tm_min);
      return at.tm_yday == today.tm_yday ? clock : i18n::tr("settings.power.yesterday", "time", clock);
    }

    std::string entryCause(const SystemExplainEntry& entry) {
      if (entry.note == "undo") {
        return i18n::tr("settings.power.cause-undo");
      }
      if (entry.note == "turned off") {
        return i18n::tr("settings.power.cause-turned-off");
      }
      if (entry.note.starts_with("you changed")) {
        return i18n::tr("settings.power.cause-you-mode");
      }
      for (const auto& [name, value] : entry.trigger) {
        if (name == "power.on_battery" && value == "false") {
          return i18n::tr("settings.power.cause-plugged");
        }
      }
      for (const auto& [name, value] : entry.trigger) {
        if (name == "battery.percent") {
          return i18n::tr("settings.power.cause-battery", "percent", value);
        }
      }
      return {};
    }

    // "What the desktop did" (docs/system-health.md §6): the last 24 h of rule actions, newest first, Undo while one runs.
    // The Activity name of the app that owns `unit`, else the unit name.
    std::string appForUnit(const ActivityMonitor* activity, const std::string& unit) {
      if (activity != nullptr) {
        for (const auto& row : activity->activity().rows()) {
          if (std::ranges::contains(row.units, unit)) {
            return identityFor(row).name;
          }
        }
      }
      return unit;
    }

    void addWhatHappened(Flex& content, SystemHealthService& health, const ActivityMonitor* activity, float scale) {
      if (health.entries().empty()) {
        return;
      }
      addHeader(content, i18n::tr("settings.power.what-happened"), nullptr, scale);
      Flex* card = addCard(content, scale);
      std::size_t shown = 0;
      for (const auto& entry : health.entries()) {
        if (++shown > 5) {
          break;
        }
        const char* key = entry.event == "started" ? "settings.power.event-on"
            : entry.event == "undone"              ? "settings.power.event-undone"
                                                   : "settings.power.event-off";
        std::string sub = entryTime(entry.timeUsec);
        if (const std::string cause = entryCause(entry); !cause.empty()) {
          sub += " · " + cause;
        }
        std::string rule = ruleLabel(entry.rule);
        if (!entry.instance.empty()) {
          rule = i18n::tr("settings.power.rule-for-app", "rule", rule, "app", appForUnit(activity, entry.instance));
        }
        auto r = row(scale);
        r->addChild(twoLines(i18n::tr(key, "rule", rule), std::move(sub), scale));
        if (health.running(entry)) {
          r->addChild(plainButton(i18n::tr("settings.power.undo"), ButtonVariant::Default, scale,
                                  [&health, id = entry.id] { health.undo(id); }));
        }
        card->addChild(std::move(r));
      }
    }

    // Labels for logind Handle* values; a value set outside Settings keeps its raw name.
    std::string handleLabel(const std::string& value) {
      static constexpr std::string_view kKnown[] = {"suspend", "hibernate", "suspend-then-hibernate", "lock", "ignore",
                                                    "poweroff"};
      if (std::ranges::contains(kKnown, value)) {
        return i18n::tr("settings.power.action." + value);
      }
      return value;
    }

    // A select that writes key=value through the root helper; disabled until the helper is installed.
    void addChoiceRow(Flex& card, PowerConfigService& config, const std::string& label, const std::string& key,
                      const std::vector<std::string>& values,
                      const std::function<std::string(const std::string&)>& labelFor, const std::string& current,
                      float scale) {
      std::vector<std::string> labels;
      std::optional<std::size_t> selected;
      for (std::size_t i = 0; i < values.size(); ++i) {
        labels.push_back(labelFor(values[i]));
        if (values[i] == current) {
          selected = i;
        }
      }
      auto r = row(scale);
      r->addChild(title(label, scale));
      r->addChild(ui::select({
          .options = std::move(labels),
          .selectedIndex = selected,
          .clearSelection = !selected.has_value(),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = config.state().helperInstalled,
          // The width of every Settings select (settings_control_factory.cpp).
          .width = 190.0F * scale,
          .onSelectionChanged =
              [&config, key, values, current](std::size_t index, std::string_view /*label*/) {
                if (index < values.size() && values[index] != current) {
                  config.set(key, values[index]);
                }
              },
      }));
      card.addChild(std::move(r));
    }

    void addHandleRow(Flex& card, PowerConfigService& config, const std::string& labelKey, const std::string& key,
                      const std::string& current, bool allowPoweroff, float scale) {
      const PowerConfigState& state = config.state();
      std::vector<std::string> values{"suspend"};
      if (config.hibernateVerified()) {
        values.emplace_back("hibernate");
      }
      if (config.hibernateVerified() && state.canSuspendThenHibernate) {
        values.emplace_back("suspend-then-hibernate");
      }
      values.insert(values.end(), {"lock", "ignore"});
      if (allowPoweroff) {
        values.emplace_back("poweroff");
      }
      if (!current.empty() && !std::ranges::contains(values, current)) {
        values.push_back(current);
      }
      addChoiceRow(card, config, i18n::tr(labelKey), key, values, handleLabel, current, scale);
    }

    void addLidButtonsCard(Flex& content, PowerConfigService& config, float scale) {
      const PowerConfigState& state = config.state();
      addHeader(content, i18n::tr("settings.power.lid-buttons"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (state.hasLid) {
        addHandleRow(*card, config, "settings.power.lid-battery", "lid-battery", state.lidBattery, false, scale);
        // Unset (logind returns "") follows HandleLidSwitch (logind.conf(5)).
        addHandleRow(*card, config, "settings.power.lid-power", "lid-power",
                     state.lidPower.empty() ? state.lidBattery : state.lidPower, false, scale);
        addHandleRow(*card, config, "settings.power.lid-docked", "lid-docked", state.lidDocked, false, scale);
      }
      addHandleRow(*card, config, "settings.power.power-key", "power-key", state.powerKey, true, scale);
    }

    std::string gib(std::uint64_t bytes) { return std::format("{:.0f} GB", static_cast<double>(bytes) / (1ULL << 30)); }

    void addSleepCard(Flex& content, PowerConfigService& config, float scale) {
      const PowerConfigState& state = config.state();
      addHeader(content, i18n::tr("settings.power.sleep"), nullptr, scale);
      Flex* card = addCard(content, scale);
      const auto modeLabel = [](const std::string& mode) {
        return mode == "s2idle" || mode == "shallow" || mode == "deep" ? i18n::tr("settings.power.sleep-mode-" + mode)
                                                                        : mode;
      };
      if (state.sleepModes.size() > 1) {
        addChoiceRow(*card, config, i18n::tr("settings.power.sleep-mode"), "sleep-mode", state.sleepModes, modeLabel,
                     state.sleepMode, scale);
      } else if (!state.sleepMode.empty()) {
        addValueRow(*card, i18n::tr("settings.power.sleep-mode"), modeLabel(state.sleepMode), scale);
      }
      if (config.hibernateVerified() && state.canSuspendThenHibernate) {
        addChoiceRow(*card, config, i18n::tr("settings.power.hibernate-after"), "hibernate-delay",
                     {"off", "30min", "1h", "2h", "3h"},
                     [](const std::string& v) { return i18n::tr("settings.power.delay." + v); }, state.hibernateDelay,
                     scale);
      }
    }

    // Each check names what blocks hibernation on this machine; swap below RAM size gets a fix (andromeda-power-config).
    void addHibernateCard(Flex& content, PowerConfigService& config, float scale) {
      const PowerConfigState& state = config.state();
      addHeader(content, i18n::tr("settings.power.hibernate"), nullptr, scale);
      Flex* card = addCard(content, scale);
      const bool swapOk = state.diskSwapBytes >= state.ramBytes;
      // logind's CanHibernate already checks that swap can hold the current memory; full-RAM swap is advice.
      const bool checksPass = !state.lockdown && state.resumeConfigured && !state.restartNeeded && state.canHibernate;
      // logind hibernates on these by itself; without a proven resume that can lose the session.
      std::vector<std::string> unsafeKeys;
      if (!config.hibernateVerified()) {
        const std::pair<const char*, const std::string*> handles[] = {
            {"lid-battery", &state.lidBattery}, {"lid-power", &state.lidPower},
            {"lid-docked", &state.lidDocked}, {"power-key", &state.powerKey}};
        for (const auto& [key, value] : handles) {
          if (*value == "hibernate" || *value == "suspend-then-hibernate") {
            unsafeKeys.emplace_back(key);
          }
        }
      }
      if (!unsafeKeys.empty()) {
        auto warnRow = row(scale);
        warnRow->addChild(title(i18n::tr("settings.power.unsafe-hibernate"), scale));
        if (state.helperInstalled) {
          warnRow->addChild(plainButton(i18n::tr("settings.power.use-sleep"), ButtonVariant::Default, scale,
                                        [&config, unsafeKeys] {
                                          for (const std::string& key : unsafeKeys) {
                                            config.set(key, "suspend");
                                          }
                                        }));
        }
        card->addChild(std::move(warnRow));
      }
      const char* status = config.hibernateVerified() ? "settings.power.hibernate-ready"
          : checksPass                                ? "settings.power.hibernate-needs-test"
                                                      : "settings.power.hibernate-not-ready";
      addValueRow(*card, i18n::tr("settings.power.hibernate-status"), i18n::tr(status), scale);
      if (state.lockdown) {
        addValueRow(*card, i18n::tr("settings.power.check-lockdown"), i18n::tr("settings.power.check-lockdown-on"),
                    scale);
        return;
      }
      // Guided setup: numbered steps, each with its status or the one button that does it.
      int stepNumber = 0;
      const auto stepTitle = [&stepNumber](const std::string& text) {
        return i18n::tr("settings.power.step", "n", std::to_string(++stepNumber), "text", text);
      };
      const auto addStep = [&](const std::string& text, bool done, const std::string& action,
                               std::function<void()> onClick) {
        if (done || !state.helperInstalled) {
          addValueRow(*card, stepTitle(text),
                      done ? i18n::tr("settings.power.step-done") : i18n::tr("settings.power.check-resume-missing"),
                      scale);
          return;
        }
        auto r = row(scale);
        r->addChild(title(stepTitle(text), scale));
        r->addChild(plainButton(i18n::tr(action), ButtonVariant::Default, scale, std::move(onClick)));
        card->addChild(std::move(r));
      };
      const std::uint64_t neededGib = (state.ramBytes + (1ULL << 30) - 1) >> 30;
      addStep(i18n::tr("settings.power.check-swap", "swap", gib(state.diskSwapBytes), "ram", gib(state.ramBytes)), swapOk,
              "settings.power.make-swap", [&config, neededGib] { config.set("swapfile", std::to_string(neededGib)); });
      // UEFI + mkinitcpio: the systemd initramfs resumes from HibernateLocation, also from a swap file.
      const std::string resumeKey = state.canSwitchInitramfs ? "initramfs-systemd" : "resume-apply";
      addStep(i18n::tr("settings.power.check-resume"), state.resumeConfigured || state.restartNeeded,
              "settings.power.set-up-resume", [&config, resumeKey] { config.set(resumeKey, "auto"); });
      if (state.restartNeeded) {
        addStep(i18n::tr("settings.power.step-restart"), false, "settings.power.restart", [&config] { config.reboot(); });
      }
      // Exit 3: the helper does not support this setup; its reason tells the user what to do by hand.
      if (state.lastExit == 3 && !state.lastError.empty()) {
        addValueRow(*card, i18n::tr("settings.power.not-supported"), state.lastError, scale);
        if (!state.lastGuide.empty()) {
          auto guideRow = row(scale);
          guideRow->addChild(title(i18n::tr("settings.power.guide"), scale));
          guideRow->addChild(plainButton(i18n::tr("settings.power.open-guide"), ButtonVariant::Default, scale,
                                         [url = state.lastGuide] {
                                           (void)process::runAsync(std::vector<std::string>{"xdg-open", url});
                                         }));
          card->addChild(std::move(guideRow));
        }
      }
      if (!checksPass) {
        addValueRow(*card, stepTitle(i18n::tr("settings.power.trial-start")), i18n::tr("settings.power.trial-wait"),
                    scale);
        return;
      }
      auto trialRow = row(scale);
      // An "ok" from before a kernel, command line or swap change counts as never tested.
      const std::string trialKey = config.hibernateVerified() ? "ok"
          : state.trial == "pending" || state.trial == "failed" ? state.trial
                                                                : "never";
      trialRow->addChild(title(stepTitle(i18n::tr("settings.power.trial-" + trialKey)), scale));
      if (trialKey == "never" || trialKey == "failed") {
        trialRow->addChild(plainButton(i18n::tr("settings.power.trial-start"), ButtonVariant::Default, scale,
                                       [&config] { config.startTrial(); }));
      }
      card->addChild(std::move(trialRow));
    }

    void addBatteryCard(Flex& content, UPowerService& upower, const UPowerDeviceInfo& battery, float scale) {
      const UPowerChargeLimitState& limit = battery.chargeLimit;
      const auto health = battery.healthPercent();
      const bool showLimit = limit.supported && limit.enabledAvailable;
      if (!showLimit && !health.has_value() && battery.chargeCycles < 0) {
        return;
      }
      addHeader(content, i18n::tr("settings.power.battery-health"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (showLimit) {
        auto r = row(scale);
        const std::string label = limit.configuredEnd.has_value()
            ? i18n::tr("settings.power.limit-to", "percent", std::to_string(*limit.configuredEnd))
            : i18n::tr("settings.power.limit");
        r->addChild(title(label, scale));
        r->addChild(ui::toggle({
            .checked = limit.requestedEnabled.value_or(limit.enabled),
            .enabled = !limit.requestPending,
            .scale = scale,
            .onChange = [&upower, path = battery.path](bool on) { (void)upower.enableChargeThreshold(path, on); },
        }));
        card->addChild(std::move(r));
      }
      if (health.has_value()) {
        addValueRow(*card, i18n::tr("settings.power.capacity"), std::format("{:.0f}%", std::round(*health)), scale);
      }
      if (battery.chargeCycles >= 0) {
        addValueRow(*card, i18n::tr("settings.power.cycle-count"), std::to_string(battery.chargeCycles), scale);
      }
    }

    // History for the open range; UPower keeps it, so a fetch per range and 5 min is enough.
    struct HistoryCache {
      bool tenDays = false;
      bool pending = false;
      std::int64_t fetchedAt = 0;
      std::vector<UPowerHistoryPoint> charge;
      std::vector<UPowerHistoryPoint> rate;
    };
    HistoryCache& historyCache() {
      static HistoryCache cache;
      return cache;
    }

    std::int64_t unixNow() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    constexpr std::uint32_t kDay = 24 * 3600;

    void fetchHistory(UPowerService& upower, const std::string& path, bool tenDays, std::function<void()> rebuild) {
      HistoryCache& cache = historyCache();
      if (cache.pending || (cache.tenDays == tenDays && unixNow() - cache.fetchedAt < 300)) {
        return;
      }
      cache.pending = true;
      const std::uint32_t span = tenDays ? 10 * kDay : kDay;
      auto remaining = std::make_shared<int>(2);
      const auto finish = [remaining, tenDays, rebuild] {
        if (--*remaining > 0) {
          return;
        }
        HistoryCache& c = historyCache();
        c.pending = false;
        c.tenDays = tenDays;
        c.fetchedAt = unixNow();
        if (rebuild) {
          rebuild();
        }
      };
      upower.requestHistory(path, "charge", span, 200, [finish](std::vector<UPowerHistoryPoint> points) {
        historyCache().charge = std::move(points);
        finish();
      });
      upower.requestHistory(path, "rate", span, 200, [finish](std::vector<UPowerHistoryPoint> points) {
        historyCache().rate = std::move(points);
        finish();
      });
    }

    // The compositor records screen time (it owns outputs and input); the page only reads its snapshot.
    ScreenActivityLog readScreenActivity() {
      ScreenActivityLog log;
      std::ifstream in(screenActivityPath());
      std::stringstream text;
      text << in.rdbuf();
      log.parse(text.str());
      return log;
    }

    std::string hoursMinutes(std::int64_t seconds) {
      return formatDuration(std::chrono::seconds{std::max<std::int64_t>(seconds, 0)});
    }

    std::unique_ptr<Flex> statTile(std::string value, std::string caption, float scale) {
      auto tile = ui::column({
          .align = FlexAlign::Start, .gap = Style::spaceXs * scale, .padding = Style::spaceMd * scale, .flexGrow = 1.0F,
      });
      tile->addChild(ui::label({
          .text = std::move(value),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
      }));
      tile->addChild(ui::label({
          .text = std::move(caption),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      return tile;
    }

    std::unique_ptr<Flex> hairline(float scale, bool vertical) {
      return ui::row({
          .fill = colorSpecFromRole(ColorRole::Separator),
          .fillWidth = !vertical,
          .width = vertical ? std::optional<float>(scale) : std::nullopt,
          .height = vertical ? std::nullopt : std::optional<float>(scale),
      });
    }

    // "Today 11:02", "Yesterday 11:02", or "Nov 3 11:02".
    std::string dayAndTime(std::time_t when) {
      const std::time_t now = std::time(nullptr);
      std::tm at{};
      std::tm today{};
      localtime_r(&when, &at);
      localtime_r(&now, &today);
      const std::string clock = std::format("{:02}:{:02}", at.tm_hour, at.tm_min);
      if (at.tm_year == today.tm_year && at.tm_yday == today.tm_yday) {
        return i18n::tr("settings.power.today", "time", clock);
      }
      if (at.tm_year == today.tm_year && at.tm_yday + 1 == today.tm_yday) {
        return i18n::tr("settings.power.yesterday", "time", clock);
      }
      char date[32] = {};
      std::strftime(date, sizeof(date), "%b %e", &at);
      return std::format("{} {}", date, clock);
    }

    // Battery drawn as in mockup A: outline, level fill in the state color, terminal nub.
    std::unique_ptr<Flex> batteryShape(const UPowerState& state, float scale) {
      const float w = 40.0F * scale;
      const float h = 20.0F * scale;
      const float inset = 3.0F * scale;
      const ColorRole role = state.percentage <= 10.0 ? ColorRole::Danger
          : state.percentage <= 20.0 && state.state != BatteryState::Charging ? ColorRole::Warning
                                                                              : ColorRole::Success;
      auto body = ui::node({});
      body->setSize(w, h);
      body->addChild(ui::box({
          .border = colorSpecFromRole(ColorRole::Label3), .borderWidth = 1.5F * scale, .radius = 6.0F * scale,
          .width = w, .height = h, .participatesInLayout = false,
      }));
      const float level = static_cast<float>(std::clamp(state.percentage, 0.0, 100.0) / 100.0);
      if (level > 0.0F) {
        auto fill = ui::box({
            .fill = colorSpecFromRole(role), .radius = 3.0F * scale, .width = std::max(2.0F * scale, (w - 2.0F * inset) * level),
            .height = h - 2.0F * inset, .participatesInLayout = false,
        });
        fill->setPosition(inset, inset);
        body->addChild(std::move(fill));
      }
      auto shape = ui::row({.align = FlexAlign::Center, .gap = 2.0F * scale});
      shape->addChild(std::move(body));
      shape->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Label3), .radius = scale, .width = 2.0F * scale, .height = 8.0F * scale,
      }));
      return shape;
    }

    // Hero card of mockup A: battery, percent and state, last full charge, then the range's four figures.
    void addBatteryHero(Flex& content, const UPowerState& state, const SettingsPowerContext& ctx, bool tenDays) {
      const float scale = ctx.scale;
      const std::int64_t now = unixNow();
      const std::int64_t span = tenDays ? 10 * kDay : kDay;
      std::int64_t on = 0;
      std::int64_t idle = 0;
      for (const auto& hour : readScreenActivity().hours(now - span, now, now)) {
        on += hour.onSeconds;
        idle += hour.idleSeconds;
      }
      double rateSum = 0.0;
      int rateCount = 0;
      for (const auto& point : historyCache().rate) {
        if (point.state == BatteryState::Discharging && point.value > 0.0) {
          rateSum += point.value;
          ++rateCount;
        }
      }
      std::uint32_t fullAt = 0;
      for (const auto& point : historyCache().charge) {
        if (point.state == BatteryState::FullyCharged || point.value >= 99.5) {
          fullAt = std::max(fullAt, point.time);
        }
      }

      Flex* card = addCard(content, scale);
      auto top = ui::row({
          .align = FlexAlign::Center, .gap = Style::spaceMd * scale, .padding = Style::spaceLg * scale, .fillWidth = true,
      });
      top->addChild(batteryShape(state, scale));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(ui::label({
          .text = std::format("{:.0f} %", state.percentage), .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
      }));
      copy->addChild(ui::label({
          .text = batteryStatus(state), .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      top->addChild(std::move(copy));
      if (fullAt > 0) {
        auto full = ui::column({.align = FlexAlign::End});
        full->addChild(ui::label({
            .text = i18n::tr("settings.power.last-full-charge"), .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
        }));
        full->addChild(ui::label({
            .text = dayAndTime(static_cast<std::time_t>(fullAt)), .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label2),
        }));
        top->addChild(std::move(full));
      }
      card->addChild(std::move(top));
      card->addChild(hairline(scale, false));

      auto tiles = ui::row({.align = FlexAlign::Stretch, .fillWidth = true});
      tiles->addChild(statTile(hoursMinutes(on), i18n::tr("settings.power.screen-on"), scale));
      tiles->addChild(hairline(scale, true));
      tiles->addChild(statTile(hoursMinutes(idle), i18n::tr("settings.power.screen-idle"), scale));
      tiles->addChild(hairline(scale, true));
      tiles->addChild(statTile(
          rateCount > 0 ? std::format("{:.1f} W", rateSum / rateCount) : std::string("–"),
          i18n::tr("settings.power.average-draw"), scale
      ));
      tiles->addChild(hairline(scale, true));
      tiles->addChild(statTile(
          state.timeToEmpty > 0 ? hoursMinutes(state.timeToEmpty) : std::string("–"), i18n::tr("settings.power.time-left"),
          scale
      ));
      card->addChild(std::move(tiles));
    }

    // Plot of mockup A: y labels in a left gutter with a grid line at each, the plot, and x labels below. `xLabels`
    // are (fraction of the plot width, text).
    std::unique_ptr<Flex> chartFrame(std::unique_ptr<Node> plot, float plotWidth, float plotHeight,
                                     const std::array<std::string, 3>& yLabels,
                                     const std::vector<std::pair<float, std::string>>& xLabels, float scale) {
      const float gutter = 32.0F * scale;
      const float labelBox = 32.0F * scale;
      const float lineHeight = 16.0F * scale;
      const auto caption = [scale](std::string text, FlexJustify justify, float width) {
        auto box = ui::row({.align = FlexAlign::Center, .justify = justify, .width = width, .height = 16.0F * scale});
        box->addChild(ui::label({
            .text = std::move(text), .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Label3),
        }));
        box->setParticipatesInLayout(false);
        return box;
      };
      auto area = ui::node({});
      area->setSize(gutter + plotWidth, plotHeight);
      for (std::size_t i = 0; i < yLabels.size(); ++i) {
        const float y = plotHeight * (1.0F - static_cast<float>(i) / static_cast<float>(yLabels.size() - 1));
        auto grid = ui::box({
            .fill = colorSpecFromRole(ColorRole::Separator), .width = plotWidth, .height = scale,
            .participatesInLayout = false,
        });
        grid->setPosition(gutter, std::min(y, plotHeight - scale));
        area->addChild(std::move(grid));
        auto label = caption(yLabels[i], FlexJustify::End, gutter - 6.0F * scale);
        label->setPosition(0.0F, std::clamp(y - lineHeight * 0.5F, -lineHeight * 0.5F, plotHeight - lineHeight * 0.5F));
        area->addChild(std::move(label));
      }
      plot->setPosition(gutter, 0.0F);
      plot->setParticipatesInLayout(false);
      area->addChild(std::move(plot));

      auto axis = ui::node({});
      axis->setSize(gutter + plotWidth, lineHeight);
      for (const auto& [fraction, text] : xLabels) {
        const bool last = fraction >= 1.0F;
        auto label = caption(text, last ? FlexJustify::End : FlexJustify::Center, labelBox);
        label->setPosition(gutter + fraction * plotWidth - (last ? labelBox : labelBox * 0.5F), 0.0F);
        axis->addChild(std::move(label));
      }
      auto frame = ui::column({.gap = Style::spaceXs * scale});
      frame->addChild(std::move(area));
      frame->addChild(std::move(axis));
      return frame;
    }

    // Clock hours 00/06/12/18 (or every other midnight over 10 days) inside the range, then "now" at the end.
    std::vector<std::pair<float, std::string>> timeLabels(std::int64_t now, std::int64_t span, bool tenDays) {
      std::vector<std::pair<float, std::string>> labels;
      const std::int64_t from = now - span;
      const std::int64_t step = tenDays ? 2 * kDay : 6 * 3600;
      std::time_t t = static_cast<std::time_t>(from);
      std::tm local{};
      localtime_r(&t, &local);
      local.tm_min = 0;
      local.tm_sec = 0;
      local.tm_hour = tenDays ? 0 : local.tm_hour - local.tm_hour % 6;
      for (std::int64_t at = std::mktime(&local); at < now; at += step) {
        const float fraction = static_cast<float>(at - from) / static_cast<float>(span);
        if (fraction < 0.0F || fraction > 0.92F) {
          continue;
        }
        std::time_t when = static_cast<std::time_t>(at);
        std::tm tm{};
        localtime_r(&when, &tm);
        char text[16] = {};
        std::strftime(text, sizeof(text), tenDays ? "%a" : "%H", &tm);
        labels.emplace_back(fraction, text);
      }
      labels.emplace_back(1.0F, i18n::tr("settings.power.now"));
      return labels;
    }

    // Level line over charging bands, then screen on / idle bars: 24 bars per hour, or 10 per day.
    void addCharts(Flex& content, const SettingsPowerContext& ctx, bool tenDays) {
      const float scale = ctx.scale;
      const float width = (metrics::kContentMaxWidth - 4.0F * Style::spaceLg - 32.0F) * scale; // card and body padding, y-axis gutter
      const float levelHeight = 120.0F * scale;
      const float barsHeight = 92.0F * scale;
      const std::int64_t now = unixNow();
      const std::uint32_t span = tenDays ? 10 * kDay : kDay;
      const BatteryLevelSeries level =
          batteryLevelSeries(historyCache().charge, static_cast<std::uint32_t>(now), span, tenDays ? 120 : 96);
      const auto xLabels = timeLabels(now, span, tenDays);

      Flex* card = addCard(content, scale);
      auto body = ui::column({.gap = Style::spaceSm * scale, .padding = Style::spaceLg * scale, .fillWidth = true});
      const auto heading = [scale](std::string text, std::string note) {
        auto r = ui::row({.align = FlexAlign::Center, .fillWidth = true});
        r->addChild(ui::label({
            .text = std::move(text), .fontSize = Style::fontSizeCaption * scale, .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::Label2), .flexGrow = 1.0F,
        }));
        r->addChild(ui::label({
            .text = std::move(note), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label3),
        }));
        return r;
      };
      body->addChild(heading(i18n::tr("settings.power.battery-level"), i18n::tr("settings.power.charging-shaded")));
      if (level.level.empty()) {
        body->addChild(ui::label({
            .text = i18n::tr("settings.power.no-history"), .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label3), .height = levelHeight,
        }));
      } else {
        auto plot = ui::node({});
        plot->setSize(width, levelHeight);
        // Charging spans are hatched (mockup A): 2 px stripes at 45 degrees every 6 px, clipped to the span.
        const float stripe = 2.0F * scale;
        const float pitch = 6.0F * scale;
        for (const auto& [from, to] : level.charging) {
          const float bandWidth = (to - from) * width;
          auto band = ui::node({});
          band->setSize(bandWidth, levelHeight);
          band->setPosition(from * width, 0.0F);
          band->setClipChildren(true);
          band->setParticipatesInLayout(false);
          for (float x = -2.0F * levelHeight; x < bandWidth + 2.0F * levelHeight; x += pitch) {
            auto line = ui::box({
                .fill = colorSpecFromRole(ColorRole::Success), .width = stripe, .height = 3.0F * levelHeight,
                .opacity = 0.14F, .participatesInLayout = false,
            });
            line->setPosition(x, -levelHeight);
            line->setRotation(0.785398F);
            band->addChild(std::move(line));
          }
          plot->addChild(std::move(band));
        }
        auto graph = std::make_unique<Graph>();
        graph->setColor(colorSpecFromRole(ColorRole::Success));
        graph->setFillOpacity(0.18F);
        graph->setValues(level.level);
        graph->setSize(width * (1.0F - level.startFraction), levelHeight);
        graph->setPosition(width * level.startFraction, 0.0F);
        graph->setParticipatesInLayout(false);
        plot->addChild(std::move(graph));
        body->addChild(chartFrame(std::move(plot), width, levelHeight, {"0%", "50%", "100%"}, xLabels, scale));
      }

      body->addChild(heading(
          i18n::tr("settings.power.screen-activity"),
          i18n::tr(tenDays ? "settings.power.minutes-per-day" : "settings.power.minutes-per-hour")
      ));
      const std::vector<ScreenActivityLog::Hour> hours = readScreenActivity().hours(now - span, now, now);
      const std::size_t bucketHours = tenDays ? 24 : 1;
      const std::size_t buckets = tenDays ? 10 : 24;
      // hours() starts at the hour that holds now - span, so it has one more hour than the buckets: skip the oldest.
      const std::size_t first = hours.size() > buckets * bucketHours ? hours.size() - buckets * bucketHours : 0;
      const float bucketSeconds = static_cast<float>(bucketHours * 3600);
      const float gap = 4.0F * scale;
      const float barWidth = (width - gap * static_cast<float>(buckets - 1)) / static_cast<float>(buckets);
      auto bars = ui::node({});
      bars->setSize(width, barsHeight);
      for (std::size_t b = 0; b < buckets; ++b) {
        std::int64_t on = 0;
        std::int64_t idle = 0;
        std::int64_t start = 0;
        for (std::size_t h = first + b * bucketHours; h < first + (b + 1) * bucketHours && h < hours.size(); ++h) {
          start = start == 0 ? hours[h].start : start;
          on += hours[h].onSeconds;
          idle += hours[h].idleSeconds;
        }
        if (start == 0) {
          continue;
        }
        double rateSum = 0.0;
        int rateCount = 0;
        for (const auto& point : historyCache().rate) {
          if (static_cast<std::int64_t>(point.time) >= start && static_cast<std::int64_t>(point.time) < start + static_cast<std::int64_t>(bucketSeconds)
              && point.state == BatteryState::Discharging && point.value > 0.0) {
            rateSum += point.value;
            ++rateCount;
          }
        }
        std::string when;
        if (tenDays) {
          when = dayAndTime(static_cast<std::time_t>(start));
          when = when.substr(0, when.rfind(' '));
        } else {
          std::time_t t = static_cast<std::time_t>(start);
          std::tm tm{};
          localtime_r(&t, &tm);
          when = std::format("{:02}:00–{:02}:00", tm.tm_hour, (tm.tm_hour + 1) % 24);
        }
        std::string tip = i18n::tr("settings.power.bar-on", "range", when, "time", hoursMinutes(on));
        if (rateCount > 0) {
          tip += std::format(" · {:.1f} W", rateSum / rateCount);
        }

        Box* onBar = nullptr;
        auto column = ui::inputArea({
            .tooltip = std::move(tip),
            .width = barWidth,
            .height = barsHeight,
            .participatesInLayout = false,
        });
        const float onHeight = barsHeight * std::min(1.0F, static_cast<float>(on) / bucketSeconds);
        const float idleHeight =
            std::min(barsHeight - onHeight, barsHeight * static_cast<float>(idle) / bucketSeconds);
        if (idleHeight >= 1.0F) {
          auto box = ui::box({
              .fill = colorSpecFromRole(ColorRole::Accent), .radius = 2.0F * scale, .width = barWidth,
              .height = idleHeight, .opacity = 0.25F, .participatesInLayout = false,
          });
          box->setPosition(0.0F, barsHeight - onHeight - idleHeight);
          column->addChild(std::move(box));
        }
        if (onHeight >= 1.0F) {
          auto box = ui::box({
              .out = &onBar, .fill = colorSpecFromRole(ColorRole::Accent), .radius = 2.0F * scale, .width = barWidth,
              .height = onHeight, .opacity = 0.55F, .participatesInLayout = false,
          });
          box->setPosition(0.0F, barsHeight - onHeight);
          column->addChild(std::move(box));
        }
        if (onBar != nullptr) {
          column->setOnEnter([onBar](const InputArea::PointerData&) { onBar->setOpacity(1.0F); });
          column->setOnLeave([onBar] { onBar->setOpacity(0.55F); });
        }
        column->setPosition(static_cast<float>(b) * (barWidth + gap), 0.0F);
        bars->addChild(std::move(column));
      }
      const std::array<std::string, 3> barLabels =
          tenDays ? std::array<std::string, 3>{"0h", "12h", "24h"} : std::array<std::string, 3>{"0m", "30m", "60m"};
      body->addChild(chartFrame(std::move(bars), width, barsHeight, barLabels, xLabels, scale));
      // Legend (mockup A): 8 px swatches in the chart colors.
      auto legend = ui::row({.align = FlexAlign::Center, .gap = Style::spaceLg * scale});
      const auto key = [scale](ColorRole role, float opacity, std::string text) {
        auto item = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        item->addChild(ui::box({
            .fill = colorSpecFromRole(role), .radius = 2.0F * scale, .width = 8.0F * scale, .height = 8.0F * scale,
            .opacity = opacity,
        }));
        item->addChild(ui::label({
            .text = std::move(text), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2),
        }));
        return item;
      };
      legend->addChild(key(ColorRole::Accent, 1.0F, i18n::tr("settings.power.screen-on")));
      legend->addChild(key(ColorRole::Accent, 0.25F, i18n::tr("settings.power.screen-idle")));
      legend->addChild(key(ColorRole::Success, 1.0F, i18n::tr("settings.power.battery-level")));
      body->addChild(std::move(legend));
      card->addChild(std::move(body));
    }

    // Apps card (mockup A): the Activity battery ranking, or screen time, or windowless apps; top 4 and "All apps".
    // Screen-on time needs history back to the last full charge, whatever the chart's range: its own 10-day fetch.
    struct SotCache {
      bool pending = false;
      std::int64_t fetchedAt = 0;
      std::vector<UPowerHistoryPoint> charge;
    };
    SotCache& sotCache() {
      static SotCache cache;
      return cache;
    }

    void fetchSotHistory(UPowerService& upower, const std::string& path, std::function<void()> rebuild) {
      SotCache& cache = sotCache();
      if (cache.pending || unixNow() - cache.fetchedAt < 300) {
        return;
      }
      cache.pending = true;
      upower.requestHistory(path, "charge", 10 * kDay, 400, [rebuild](std::vector<UPowerHistoryPoint> points) {
        SotCache& c = sotCache();
        c.charge = std::move(points);
        c.pending = false;
        c.fetchedAt = unixNow();
        if (rebuild) {
          rebuild();
        }
      });
    }

    // -1 until the user picks a period: since unplugged on battery, since full charge on power.
    int& sotPeriodIndex() {
      static int index = -1;
      return index;
    }

    std::string clockTime(std::int64_t when) {
      const std::time_t t = static_cast<std::time_t>(when);
      std::tm local{};
      localtime_r(&t, &local);
      return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    }

    // Screen-on time since unplugged / since the last full charge: the compositor's measured state spans over the
    // battery line, the measured times and drain, then two estimates from the measured screen-on drain.
    void addScreenOnTime(Flex& content, const SettingsPowerContext& ctx, const UPowerState& state) {
      const float scale = ctx.scale;
      const std::int64_t now = unixNow();
      const bool discharging = state.state == BatteryState::Discharging;
      const int periodIndex = sotPeriodIndex() >= 0 ? sotPeriodIndex() : (discharging ? 0 : 1);
      const SotPeriod period = periodIndex == 0 ? SotPeriod::SinceUnplugged : SotPeriod::SinceFullCharge;
      const auto& points = sotCache().charge;

      auto tabs = ui::row({.justify = FlexJustify::End, .flexGrow = 1.0F});
      tabs->addChild(ui::segmented({
          .options =
              std::vector<ui::SegmentedOption>{
                  ui::SegmentedOption{.label = i18n::tr("settings.power.sot-since-unplugged")},
                  ui::SegmentedOption{.label = i18n::tr("settings.power.sot-since-full")},
              },
          .selectedIndex = static_cast<std::size_t>(periodIndex),
          .scale = scale,
          .compact = true,
          .onChange =
              [rebuild = ctx.rebuild](std::size_t index) {
                sotPeriodIndex() = static_cast<int>(index);
                if (rebuild) {
                  rebuild();
                }
              },
      }));
      addHeader(content, i18n::tr("settings.power.sot"), std::move(tabs), scale);
      Flex* card = addCard(content, scale);

      const std::optional<std::int64_t> start = sotPeriodStart(points, period);
      if (!start.has_value() || *start >= now) {
        auto note = ui::column({.padding = Style::spaceLg * scale, .fillWidth = true});
        note->addChild(ui::label({
            .text = i18n::tr(period == SotPeriod::SinceUnplugged && !points.empty() ? "settings.power.sot-plugged"
                                                                                    : "settings.power.sot-no-data"),
            .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::Label3), .maxLines = 0,
        }));
        card->addChild(std::move(note));
        return;
      }
      const ScreenActivityLog log = readScreenActivity();
      const std::vector<ScreenActivityLog::Span> spans = log.spans(*start, now);
      const ScreenOnTime sot = screenOnTime(spans, points, *start, now);

      auto top = ui::row({.align = FlexAlign::End, .gap = Style::spaceMd * scale, .padding = Style::spaceLg * scale,
                          .fillWidth = true});
      top->addChild(ui::label({
          .text = hoursMinutes(sot.onSeconds + sot.idleSeconds), .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
      }));
      top->addChild(ui::label({
          .text = i18n::tr("settings.power.sot-since", "time", dayAndTime(static_cast<std::time_t>(*start)), "used",
                           std::format("{:.0f}", sot.usedPercent)),
          .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2), .flexGrow = 1.0F,
      }));
      card->addChild(std::move(top));

      // Battery line over the period, and below it one strip of the screen's state.
      const float width = (metrics::kContentMaxWidth - 4.0F * Style::spaceLg - 32.0F) * scale;
      const float height = 96.0F * scale;
      const float stripHeight = 10.0F * scale;
      const auto span = static_cast<std::uint32_t>(now - *start);
      const BatteryLevelSeries level = batteryLevelSeries(points, static_cast<std::uint32_t>(now), span, 96);
      auto body = ui::column({.gap = Style::spaceSm * scale, .padding = Style::spaceLg * scale, .fillWidth = true});
      auto plot = ui::node({});
      plot->setSize(width, height);
      if (!level.level.empty()) {
        auto graph = std::make_unique<Graph>();
        graph->setColor(colorSpecFromRole(ColorRole::Success));
        graph->setFillOpacity(0.18F);
        graph->setValues(level.level);
        graph->setSize(width * (1.0F - level.startFraction), height);
        graph->setPosition(width * level.startFraction, 0.0F);
        graph->setParticipatesInLayout(false);
        plot->addChild(std::move(graph));
      }
      const std::vector<std::pair<float, std::string>> xLabels = {{0.0F, clockTime(*start)}, {1.0F, i18n::tr("settings.power.now")}};
      body->addChild(chartFrame(std::move(plot), width, height, {"0%", "50%", "100%"}, xLabels, scale));

      using State = ScreenActivityLog::State;
      const auto stateStyle = [](State s) -> std::pair<ColorRole, float> {
        switch (s) {
        case State::On:
          return {ColorRole::Accent, 1.0F};
        case State::Idle:
          return {ColorRole::Accent, 0.45F};
        case State::Off:
          return {ColorRole::Label3, 0.35F};
        case State::Sleep:
          return {ColorRole::Label3, 0.12F};
        }
        return {ColorRole::Label3, 0.0F};
      };
      const float gutter = 32.0F * scale;
      auto strip = ui::node({});
      strip->setSize(gutter + width, stripHeight);
      for (const auto& s : spans) {
        const float from = static_cast<float>(std::max(s.start, *start) - *start) / static_cast<float>(span);
        const float to = static_cast<float>(std::min(s.end, now) - *start) / static_cast<float>(span);
        if (to <= from) {
          continue;
        }
        const auto [role, opacity] = stateStyle(s.state);
        auto box = ui::box({.fill = colorSpecFromRole(role), .width = std::max((to - from) * width, scale),
                            .height = stripHeight, .opacity = opacity, .participatesInLayout = false});
        box->setPosition(gutter + from * width, 0.0F);
        strip->addChild(std::move(box));
      }
      body->addChild(std::move(strip));
      auto legend = ui::row({.gap = Style::spaceMd * scale, .padding = 0.0F, .fillWidth = true});
      for (const auto& [s, key] : std::array<std::pair<State, const char*>, 4>{{{State::On, "settings.power.sot-screen-on"},
                                                                                {State::Idle, "settings.power.sot-screen-idle"},
                                                                                {State::Off, "settings.power.sot-screen-off"},
                                                                                {State::Sleep, "settings.power.sot-sleep"}}}) {
        const auto [role, opacity] = stateStyle(s);
        auto item = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
        item->addChild(ui::box({.fill = colorSpecFromRole(role), .radius = 2.0F * scale, .width = 8.0F * scale,
                                .height = 8.0F * scale, .opacity = std::max(opacity, 0.25F)}));
        item->addChild(ui::label({.text = i18n::tr(key), .fontSize = Style::fontSizeCaption * scale,
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
        legend->addChild(std::move(item));
      }
      body->addChild(std::move(legend));
      card->addChild(std::move(body));
      card->addChild(hairline(scale, false));

      const auto perHour = [](double rate) { return rate > 0.0 ? std::format("{:.1f} %/h", rate) : std::string("–"); };
      auto measured = ui::row({.align = FlexAlign::Stretch, .fillWidth = true});
      measured->addChild(statTile(hoursMinutes(sot.onSeconds), i18n::tr("settings.power.sot-screen-on"), scale));
      measured->addChild(hairline(scale, true));
      measured->addChild(statTile(hoursMinutes(sot.idleSeconds), i18n::tr("settings.power.sot-screen-idle"), scale));
      measured->addChild(hairline(scale, true));
      measured->addChild(statTile(hoursMinutes(sot.offSeconds), i18n::tr("settings.power.sot-screen-off"), scale));
      measured->addChild(hairline(scale, true));
      measured->addChild(statTile(hoursMinutes(sot.sleepSeconds), i18n::tr("settings.power.sot-sleep"), scale));
      card->addChild(std::move(measured));
      card->addChild(hairline(scale, false));

      auto drain = ui::row({.align = FlexAlign::Stretch, .fillWidth = true});
      drain->addChild(statTile(perHour(sot.screenOnDrainPerHour), i18n::tr("settings.power.sot-drain-on"), scale));
      drain->addChild(hairline(scale, true));
      drain->addChild(statTile(perHour(sot.screenOffDrainPerHour), i18n::tr("settings.power.sot-drain-off"), scale));
      drain->addChild(hairline(scale, true));
      const double rate = sot.screenOnDrainPerHour;
      drain->addChild(statTile(
          rate > 0.0 ? hoursMinutes(static_cast<std::int64_t>(state.percentage / rate * 3600.0)) : std::string("–"),
          i18n::tr("settings.power.sot-est-left"), scale
      ));
      drain->addChild(hairline(scale, true));
      drain->addChild(statTile(
          rate > 0.0 ? hoursMinutes(static_cast<std::int64_t>(100.0 / rate * 3600.0)) : std::string("–"),
          i18n::tr("settings.power.sot-est-full"), scale
      ));
      card->addChild(std::move(drain));
    }

    int& appsTab() {
      static int tab = 0;
      return tab;
    }

    void addApps(Flex& content, const SettingsPowerContext& ctx, bool tenDays) {
      if (ctx.activity == nullptr) {
        return;
      }
      const float scale = ctx.scale;
      const auto& rows = ctx.activity->activity().rows();
      // One unit list and cgroup read gives the cumulative CPU time the ranking needs; at most once a minute.
      static std::int64_t sampledAt = 0;
      if (rows.empty() && unixNow() - sampledAt > 60) {
        sampledAt = unixNow();
        ctx.activity->sampleOnce(ctx.rebuild);
      }
      std::map<std::string, std::int64_t> screenSeconds;
      if (ctx.screenTime != nullptr && ctx.screenTime->enabled()) {
        for (const auto& app : ctx.screenTime->snapshot(tenDays ? 10 : 1).apps) {
          screenSeconds[app.appKey] = app.total.count();
        }
      }
      const auto onScreen = [&screenSeconds](const ActivityRow& row) -> std::int64_t {
        const auto it = screenSeconds.find(row.appId.empty() ? row.key : row.appId);
        return it == screenSeconds.end() ? 0 : it->second;
      };
      const int tab = appsTab();
      std::vector<BatteryImpact> ranked = rankBatteryImpact(rows, ctx.activityDetails, [tab](const ActivityRow& r) {
        return tab != 2 || !r.hasWindow;
      });
      // The battery tab ranks what the compositor booked per hour, closed apps included: Wh (package energy split by
      // CPU share) where andromeda-system reports RAPL, else CPU time over 10 days.
      std::vector<ActivityRow> pastRows;
      if (tab == 0) {
        std::ifstream in(appEnergyPath());
        std::stringstream text;
        text << in.rdbuf();
        AppEnergyLog log;
        log.parse(text.str());
        const std::int64_t seconds = static_cast<std::int64_t>(tenDays ? 10 * kDay : kDay);
        auto booked = log.wattHours(unixNow(), seconds);
        const bool energy = !booked.empty();
        if (!energy && tenDays) {
          booked = log.cpuSeconds(unixNow(), seconds);
        }
        if (!booked.empty()) {
          ranked = rankAppEnergy(booked, rows, pastRows, energy);
        }
      }
      if (tab == 1) {
        std::ranges::stable_sort(ranked, std::greater{}, [&](const BatteryImpact& b) { return onScreen(*b.row); });
      }
      // Tabs at the trailing edge of the section header (mockup A).
      auto tabs = ui::row({.justify = FlexJustify::End, .flexGrow = 1.0F});
      tabs->addChild(ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      ui::SegmentedOption{.label = i18n::tr("settings.power.apps-battery")},
                      ui::SegmentedOption{.label = i18n::tr("settings.power.apps-screen")},
                      ui::SegmentedOption{.label = i18n::tr("settings.power.apps-background")},
                  },
              .selectedIndex = static_cast<std::size_t>(tab),
              .scale = scale,
              .compact = true,
              .onChange =
                  [rebuild = ctx.rebuild](std::size_t index) {
                    appsTab() = static_cast<int>(index);
                    if (rebuild) {
                      rebuild();
                    }
                  },
          }));
      addHeader(content, i18n::tr("settings.power.apps"), std::move(tabs), scale);
      Flex* card = addCard(content, scale);
      std::size_t shown = 0;
      for (const BatteryImpact& impact : ranked) {
        if (++shown > 4) {
          break;
        }
        const ActivityRow* app = impact.row;
        const double share = impact.sharePercent;
        const RowIdentity id = identityFor(*app);
        const std::int64_t screen = onScreen(*app);
        std::string sub = screen > 0 ? i18n::tr("settings.power.on-screen", "time", hoursMinutes(screen))
                                     : i18n::tr("settings.power.in-background");
        auto r = row(scale);
        auto tile = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                                .width = Tokens::sizeControlS * scale, .height = Tokens::sizeControlS * scale});
        tile->addChild(ui::box({
            .fill = colorSpecFromRole(ColorRole::Accent), .radius = Style::scaledRadiusMd(scale),
            .width = Tokens::sizeControlS * scale, .height = Tokens::sizeControlS * scale, .participatesInLayout = false,
        }));
        tile->addChild(ui::label({
            .text = id.name.empty() ? std::string("?") : id.name.substr(0, firstCharBytes(id.name)), .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::OnAccent),
        }));
        r->addChild(std::move(tile));
        r->addChild(twoLines(id.name, std::move(sub), scale));
        const float meterWidth = 108.0F * scale;
        auto meter = ui::node({});
        meter->setSize(meterWidth, 4.0F * scale);
        auto track = ui::box({.fill = colorSpecFromRole(ColorRole::Label3), .radius = 2.0F * scale, .width = meterWidth,
                              .height = 4.0F * scale, .opacity = 0.25F, .participatesInLayout = false});
        auto fill = ui::box({.fill = colorSpecFromRole(ColorRole::Accent), .radius = 2.0F * scale,
                             .width = meterWidth * static_cast<float>(std::clamp(share / 100.0, 0.02, 1.0)),
                             .height = 4.0F * scale, .participatesInLayout = false});
        meter->addChild(std::move(track));
        meter->addChild(std::move(fill));
        r->addChild(std::move(meter));
        r->addChild(ui::label({
            .text = impact.wattHours >= 0.0 ? std::format("{:.1f} Wh", impact.wattHours) : std::format("{:.0f} %", share),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
        }));
        if (ctx.openActivity) {
          r->addChild(iconButton("chevron-right", id.name, scale, [open = ctx.openActivity, key = app->key] { open(key); }));
        }
        card->addChild(std::move(r));
      }
      auto all = row(scale);
      all->addChild(title(i18n::tr("settings.power.all-apps"), scale));
      if (ctx.openActivity) {
        all->addChild(iconButton("chevron-right", i18n::tr("settings.power.all-apps"), scale,
                                 [open = ctx.openActivity] { open(""); }));
      }
      card->addChild(std::move(all));
    }

  } // namespace

  void addSettingsPower(Flex& content, const SettingsPowerContext& ctx) {
    const float scale = ctx.scale;
    const UPowerDeviceInfo* battery = ctx.upower != nullptr ? ctx.upower->defaultSystemBattery() : nullptr;
    if (battery != nullptr && battery->isPresent) {
      const UPowerState& state = battery->state;
      HistoryCache& cache = historyCache();
      const bool tenDays = cache.tenDays;
      fetchHistory(*ctx.upower, battery->path, tenDays, ctx.rebuild);
      // The window adds the page title just before this content; the range toggle joins its row (mockup A).
      if (!content.children().empty()) {
        auto title = content.removeChild(content.children().back().get());
        auto titleRow = ui::row({.align = FlexAlign::Center, .fillWidth = true});
        title->setFlexGrow(1.0F);
        titleRow->addChild(std::move(title));
        titleRow->addChild(
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      ui::SegmentedOption{.label = i18n::tr("settings.power.range-day")},
                      ui::SegmentedOption{.label = i18n::tr("settings.power.range-days")},
                  },
              .selectedIndex = std::size_t{tenDays ? 1U : 0U},
              .scale = scale,
              .compact = true,
              .onChange =
                  [upower = ctx.upower, path = battery->path, rebuild = ctx.rebuild](std::size_t index) {
                    historyCache().fetchedAt = 0;
                    fetchHistory(*upower, path, index == 1, rebuild);
                  },
          })
        );
        content.addChild(std::move(titleRow));
      }
      addBatteryHero(content, state, ctx, tenDays);
      fetchSotHistory(*ctx.upower, battery->path, ctx.rebuild);
      addScreenOnTime(content, ctx, state);
      addCharts(content, ctx, tenDays);
      addApps(content, ctx, tenDays);
    }
    if (ctx.profiles != nullptr && ctx.profiles->hasStateSnapshot() && !ctx.profiles->profiles().empty()) {
      addHeader(content, i18n::tr("settings.power.energy-saving"), nullptr, scale);
      addEnergySaving(content, *ctx.profiles, ctx.health, scale);
    }
    if (ctx.health != nullptr) {
      addWhatHappened(content, *ctx.health, ctx.activity, scale);
    }
    if (battery != nullptr && battery->isPresent) {
      addBatteryCard(content, *ctx.upower, *battery, scale);
    }
    if (ctx.powerConfig != nullptr) {
      addLidButtonsCard(content, *ctx.powerConfig, scale);
      addSleepCard(content, *ctx.powerConfig, scale);
      addHibernateCard(content, *ctx.powerConfig, scale);
    }
  }

} // namespace settings
