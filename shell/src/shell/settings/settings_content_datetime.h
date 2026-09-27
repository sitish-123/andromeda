#pragma once

#include "dbus/timedate/timedate_service.h"

#include <functional>

class Flex;

namespace settings {

  struct SettingsDateTimeContext {
    float scale = 1.0F;
    const TimeDateService* timedate = nullptr;
    std::function<void(std::string)> setTimezone;
    std::function<void(bool)> setNtp;
    std::function<void(bool)> setLocalRtc;
    std::function<void(std::string)> setTimeText; // "YYYY-MM-DD HH:MM:SS" UTC
  };

  void addSettingsDateTime(Flex& content, const SettingsDateTimeContext& ctx);

} // namespace settings
