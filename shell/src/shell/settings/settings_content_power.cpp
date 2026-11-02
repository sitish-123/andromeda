#include "shell/settings/settings_content_power.h"

#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_page_rows.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <format>
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
  }

} // namespace settings
