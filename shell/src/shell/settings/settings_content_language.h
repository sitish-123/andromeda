#pragma once

#include "dbus/locale/locale_service.h"
#include "system/xkb_layout_catalog.h"

#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Flex;

namespace settings {

  struct SettingsLanguageContext {
    float scale = 1.0F;
    const LocaleService* locale = nullptr;
    const xkb::Catalog* catalog = nullptr;
    // (locale id, display name) from loadLocaleNames(); empty while it loads.
    std::span<const std::pair<std::string, std::string>> locales;
    // Full locale1 assignment list ("LANG=…", "LC_TIME=…").
    std::function<void(std::vector<std::string>)> setLocale;
    // layout, variant
    std::function<void(std::string, std::string)> setX11Layout;
    // Settings › Keyboard, where the session's input sources live.
    std::function<void()> openKeyboard;
  };

  // Language and Region page (.agent/mockups/settings-d6.html §6): Language and Region with a format preview,
  // then Keyboard.
  void addSettingsLanguage(Flex& content, const SettingsLanguageContext& ctx);

  // Every supported locale with its glibc display name; blocking (one newlocale each): run it off the main thread.
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> loadLocaleNames();

} // namespace settings
