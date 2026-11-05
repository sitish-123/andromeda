#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Flex;
class SoftwareUpdateService;

namespace settings {

  struct SoftwareUpdateActions {
    std::function<void()> check;
    std::function<void()> markNewsRead;
    std::function<void()> upgrade;
    std::function<void(bool enable)> scheduleTonight;
    std::function<void()> restart;
    std::function<void(const std::string& url)> openLink;
    // "system", "apps" or "firmware": the package list of that group.
    std::function<void(const std::string& group)> openGroup;
    std::function<void(bool on)> setCheckDaily;
  };

  // Settings › Software Update (.agent/DESIGN.md "Software Update page (U3)", direction A). `group` non-empty shows
  // that group's package list. `hookFailures`: (event, script) of user hooks that failed.
  void addSoftwareUpdate(
      Flex& content, float scale, const SoftwareUpdateService* service, std::string_view group,
      const std::vector<std::pair<std::string, std::string>>& hookFailures, bool checkDaily,
      const SoftwareUpdateActions& actions
  );

} // namespace settings
