#pragma once

class Flex;
class PowerProfilesService;
class UPowerService;

namespace settings {

  struct SettingsPowerContext {
    float scale = 1.0F;
    UPowerService* upower = nullptr;
    PowerProfilesService* profiles = nullptr;
  };

  // Battery and Power page head (direction A, .agent/DESIGN.md): battery hero, Power Mode, Battery card.
  // The page's registry rows (session panel, idle) follow it.
  void addSettingsPower(Flex& content, const SettingsPowerContext& ctx);

} // namespace settings
