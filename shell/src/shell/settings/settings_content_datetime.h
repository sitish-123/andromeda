#pragma once

#include "config/config_types.h"
#include "dbus/timedate/timedate_service.h"

#include <functional>
#include <string>
#include <vector>

class Flex;

namespace settings {

  struct SettingsDateTimeContext {
    float scale = 1.0F;
    const TimeDateService* timedate = nullptr;
    const Config* config = nullptr;
    std::function<void(std::string)> setTimezone;
    std::function<void(bool)> setNtp;
    std::function<void(std::string)> setTimeText; // "YYYY-MM-DD HH:MM:SS" UTC
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    struct Account {
      std::string name;
      // Sync state ("Synced 5 min ago").
      std::string status;
      std::function<void()> open;
    };
    std::vector<Account> accounts;
    std::function<void()> addAccount;
    // Language and Region: the region sets the first day of the week.
    std::function<void()> openRegion;
  };

  // Date and Time page (.agent/mockups/settings-d6.html §5): Date and Time, Formats, Calendar.
  void addSettingsDateTime(Flex& content, const SettingsDateTimeContext& ctx);

} // namespace settings
