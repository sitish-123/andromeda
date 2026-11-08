#pragma once

#include "config/config_types.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class Flex;

namespace settings {

  struct SettingsAutomationContext {
    float scale = 1.0F;
    const HooksConfig* hooks = nullptr;
    // Opens the rule sheet: both empty = New Rule, else the hook kind and its command index.
    std::function<void(std::optional<std::size_t> hook, std::optional<std::size_t> command)> openRule;
    // Switches the rule (hook kind index, command text) on or off: [hooks.disabled].
    std::function<void(std::size_t hook, std::string command, bool enabled)> setEnabled;
  };

  // Automation page (.agent/mockups/settings-d6.html §2): each hook command is a rule read as a sentence.
  void addSettingsAutomation(Flex& content, const SettingsAutomationContext& ctx);

  // "When the session locks" for a hook kind index.
  [[nodiscard]] std::string automationEventText(std::size_t hook);
  [[nodiscard]] std::string_view automationEventGlyph(std::size_t hook);

} // namespace settings
