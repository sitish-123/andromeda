#include "shell/settings/settings_content_power.h"

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
#include <optional>
#include <string>
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

    void addHandleRow(Flex& card, PowerConfigService& config, const std::string& labelKey, const std::string& key,
                      const std::string& current, bool allowPoweroff, float scale) {
      const PowerConfigState& state = config.state();
      std::vector<std::string> values{"suspend"};
      if (state.canHibernate) {
        values.emplace_back("hibernate");
      }
      if (state.canSuspendThenHibernate) {
        values.emplace_back("suspend-then-hibernate");
      }
      values.insert(values.end(), {"lock", "ignore"});
      if (allowPoweroff) {
        values.emplace_back("poweroff");
      }
      if (!current.empty() && !std::ranges::contains(values, current)) {
        values.push_back(current);
      }
      std::vector<std::string> labels;
      std::optional<std::size_t> selected;
      for (std::size_t i = 0; i < values.size(); ++i) {
        labels.push_back(handleLabel(values[i]));
        if (values[i] == current) {
          selected = i;
        }
      }
      auto r = row(scale);
      r->addChild(title(i18n::tr(labelKey), scale));
      r->addChild(ui::select({
          .options = std::move(labels),
          .selectedIndex = selected,
          .clearSelection = !selected.has_value(),
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = state.helperInstalled,
          .onSelectionChanged =
              [&config, key, values, current](std::size_t index, std::string_view /*label*/) {
                if (index < values.size() && values[index] != current) {
                  config.set(key, values[index]);
                }
              },
      }));
      card.addChild(std::move(r));
    }

    void addLidButtonsCard(Flex& content, PowerConfigService& config, float scale) {
      const PowerConfigState& state = config.state();
      addHeader(content, i18n::tr("settings.power.lid-buttons"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (state.hasLid) {
        addHandleRow(*card, config, "settings.power.lid-battery", "lid-battery", state.lidBattery, false, scale);
        addHandleRow(*card, config, "settings.power.lid-power", "lid-power", state.lidPower, false, scale);
        addHandleRow(*card, config, "settings.power.lid-docked", "lid-docked", state.lidDocked, false, scale);
      }
      addHandleRow(*card, config, "settings.power.power-key", "power-key", state.powerKey, true, scale);
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
    }
  }

} // namespace settings
