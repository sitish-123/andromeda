#pragma once

#include <functional>
#include <string>

class ActivityDetails;
class ActivityMonitor;
class Flex;
class ScreenTimeService;
class PowerConfigService;
class SystemHealthService;
class PowerProfilesService;
class UPowerService;

namespace settings {

  struct SettingsPowerContext {
    float scale = 1.0F;
    UPowerService* upower = nullptr;
    PowerProfilesService* profiles = nullptr;
    PowerConfigService* powerConfig = nullptr;
    // Rules and the explain log of andromeda-system's session part (docs/system-health.md §6).
    SystemHealthService* health = nullptr;
    // Apps card: the Activity ranking (system/battery_impact.h) and per-app screen time.
    ActivityMonitor* activity = nullptr;
    const ActivityDetails* activityDetails = nullptr;
    ScreenTimeService* screenTime = nullptr;
    // Opens Activity on a row key (empty: the Battery tab).
    std::function<void(const std::string&)> openActivity;
    // Rebuilds the page (battery history arrives after the first build).
    std::function<void()> rebuild;
  };

  // Battery and Power page head (direction A, .agent/DESIGN.md): battery hero, Power Mode, Battery card, Lid & Power Button.
  // The page's registry rows (session panel, idle) follow it.
  void addSettingsPower(Flex& content, const SettingsPowerContext& ctx);

} // namespace settings
