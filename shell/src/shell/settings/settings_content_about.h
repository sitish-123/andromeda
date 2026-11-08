#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

class Flex;

namespace settings {

  struct AboutHardware {
    std::string processor;
    std::string graphics;
    std::string memory;
    std::string storage;
    std::string display;
  };

  // Blocking sysfs, /proc and pci.ids reads: run it off the main thread.
  [[nodiscard]] AboutHardware readAboutHardware();

  struct AboutFacts {
    // org.freedesktop.hostname1 string properties; empty while loading or without hostnamed.
    std::map<std::string, std::string> hostname;
    std::optional<AboutHardware> hardware;
  };

  struct SettingsAboutContext {
    float scale = 1.0F;
    const AboutFacts* facts = nullptr;
    bool renaming = false;
    // The error of the last rename; empty when it worked.
    std::string renameError;
    // The Software Update row value ("Up to date", "3 updates"); empty while unknown.
    std::string updateStatus;
    // Opens Settings › Software Update.
    std::function<void()> openSoftwareUpdate;
    // Opens the rename sheet with the current device name.
    std::function<void(std::string current)> openRename;
    std::function<void()> openLegal;
    // Puts the device facts on the clipboard as plain text.
    std::function<void(std::string text)> copyInfo;
  };

  // About page (.agent/mockups/settings-d6.html §1): device card, Hardware, Software, then Legal / Copy System Info.
  void addSettingsAbout(Flex& content, const SettingsAboutContext& ctx);

  // Static hostname for a display name: "Sitish's Zenbook" -> "sitishs-zenbook"; empty when nothing is left.
  [[nodiscard]] std::string hostnameFromName(const std::string& name);

} // namespace settings
