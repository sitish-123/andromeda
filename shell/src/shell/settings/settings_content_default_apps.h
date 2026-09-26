#pragma once

#include "system/default_apps.h"
#include "system/desktop_entry.h"

#include <functional>
#include <span>
#include <string>

class Flex;

namespace settings {

  struct SettingsDefaultAppsContext {
    float scale = 1.0F;
    std::span<const DesktopEntry> entries;
    std::function<std::string(default_apps::Category)> currentDefault;
    std::function<void(default_apps::Category, std::string)> setDefault;
  };

  void addSettingsDefaultApps(Flex& content, const SettingsDefaultAppsContext& ctx);

} // namespace settings
