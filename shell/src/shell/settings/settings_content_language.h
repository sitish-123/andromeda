#pragma once

#include "dbus/locale/locale_service.h"
#include "system/xkb_layout_catalog.h"

#include <functional>

class Flex;

namespace settings {

  struct SettingsLanguageContext {
    float scale = 1.0F;
    const LocaleService* locale = nullptr;
    const xkb::Catalog* catalog = nullptr;
    std::function<void(std::string)> setLang;
    // layout, variant
    std::function<void(std::string, std::string)> setX11Layout;
  };

  void addSettingsLanguage(Flex& content, const SettingsLanguageContext& ctx);

} // namespace settings
