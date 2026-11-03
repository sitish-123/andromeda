#include "shell/settings/settings_content_power.h"

#include "core/process/process.h"
#include "dbus/logind/power_config_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_page_rows.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
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

    void addPowerMode(Flex& content, PowerProfilesService& profiles, float scale) {
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
      r->addChild(title(i18n::tr("settings.power.mode"), scale));
      r->addChild(ui::segmented({
          .options = std::move(options),
          .selectedIndex = selected,
          .scale = scale,
          .onChange = [&profiles, shown](std::size_t index) { (void)profiles.setActiveProfile(shown[index]); },
      }));
      card->addChild(std::move(r));
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
      if (!showLimit && !health.has_value()) {
        return;
      }
      addHeader(content, i18n::tr("settings.power.battery"), nullptr, scale);
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
        addValueRow(*card, i18n::tr("settings.power.health"), std::format("{:.0f}%", std::round(*health)), scale);
      }
    }

  } // namespace

  void addSettingsPower(Flex& content, const SettingsPowerContext& ctx) {
    const float scale = ctx.scale;
    const UPowerDeviceInfo* battery = ctx.upower != nullptr ? ctx.upower->defaultSystemBattery() : nullptr;
    if (battery != nullptr && battery->isPresent) {
      const UPowerState& state = battery->state;
      addHero(content,
              Hero{
                  .glyph = batteryGlyphName(state.percentage, state.state),
                  .on = true,
                  .title = std::format("{:.0f}%", state.percentage),
                  .status = batteryStatus(state),
              },
              scale);
    }
    if (ctx.profiles != nullptr && ctx.profiles->hasStateSnapshot()) {
      addPowerMode(content, *ctx.profiles, scale);
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
