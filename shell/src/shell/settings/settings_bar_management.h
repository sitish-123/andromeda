#pragma once

#include <functional>
#include <string>
#include <string_view>

struct BarConfig;
struct BarMonitorOverride;
struct DockMonitorOverride;
struct Config;
class ConfigService;
class Flex;

namespace settings {

  struct SettingsBarManagementContext {
    const Config& config;
    ConfigService* configService = nullptr;
    float scale = 1.0F;
    std::string_view searchQuery;
    std::string_view selectedSection;
    const BarConfig* selectedBar = nullptr;
    const BarMonitorOverride* selectedMonitorOverride = nullptr;
    const DockMonitorOverride* selectedDockMonitorOverride = nullptr;

    std::string& renamingBarName;
    std::string& renamingMonitorOverrideBarName;
    std::string& renamingMonitorOverrideMatch;
    std::string& renamingDockMonitorOverride;

    std::function<void()> requestRebuild;
    std::function<void(std::string, std::string)> renameBar;
    std::function<void(std::string)> deleteBar;
    std::function<void(std::string, int)> moveBar;
    std::function<void(std::string, std::string, std::string)> renameMonitorOverride;
    std::function<void(std::string, std::string)> deleteMonitorOverride;
    // Empty match selects the bar's settings for all displays.
    std::function<void(std::string)> selectMonitorOverride;
    std::function<void(std::string)> createMonitorOverride;
    std::function<void(std::string, std::string)> renameDockMonitorOverride;
    std::function<void(std::string)> deleteDockMonitorOverride;
    // Shows a destructive alert (title, body, action label); the action runs only on confirm.
    std::function<void(std::string, std::string, std::string, std::function<void()>)> confirmDelete;
  };

  void addSettingsBarManagement(Flex& content, SettingsBarManagementContext ctx);

} // namespace settings
