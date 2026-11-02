#pragma once

#include <functional>
#include <string>

class Flex;

namespace settings {

  struct SettingsAboutContext {
    float scale = 1.0F;
    bool renaming = false;
    // The error of the last rename; empty when it worked.
    std::string renameError;
    std::function<void(std::string name)> rename;
  };

  // About page head: the device name (editable) and the device facts. The page's registry rows follow it.
  void addSettingsAbout(Flex& content, const SettingsAboutContext& ctx);

  // Static hostname for a display name: "Sitish's Zenbook" -> "sitishs-zenbook"; empty when nothing is left.
  [[nodiscard]] std::string hostnameFromName(const std::string& name);

} // namespace settings
