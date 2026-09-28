#pragma once

#include "system/xkb_layout_catalog.h"
#include "wayland/settings_control.h"

#include <functional>
#include <string>

class Flex;

namespace settings {

  struct SettingsInputContext {
    float scale = 1.0F;
    const SettingsControl* input = nullptr;
    const xkb::Catalog* catalog = nullptr;
    std::function<void(std::string key, std::string value)> set;
  };

  void addSettingsInput(Flex& content, const SettingsInputContext& ctx);

} // namespace settings
