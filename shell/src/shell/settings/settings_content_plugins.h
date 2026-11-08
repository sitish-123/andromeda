#pragma once

#include "config/config_types.h"
#include "scripting/plugin_manager.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Flex;
class Node;

namespace scripting {
  struct PluginManifest;
}

namespace settings {

  class SettingsControlFactory;

  // Data + actions for the Plugins settings section. Populated by SettingsWindow
  // from the PluginManager; the section is fully custom (no registry entries).
  struct SettingsPluginsContext {
    float scale = 1.0F;
    std::string_view selectedSection;
    std::vector<scripting::PluginStatus> plugins;
    std::vector<PluginSourceConfig> sources;
    bool pluginsLoading = false;

    std::function<void(std::string id, bool enable)> setEnabled;
    // True while a git-source plugin's runtime export runs in the background; the row
    // shows a spinner in place of the toggle until it lands.
    std::function<bool(const std::string& id)> isEnabling;
    std::function<void()> addSource;
    std::function<void(PluginSourceConfig source, bool enabled)> setSourceEnabled;
    std::function<void(PluginSourceConfig source)> editSource;
    std::function<void(std::string source)> updateSource;

    // Background auto-update scope for git sources; drives the "auto-update plugins"
    // dropdown. setAutoUpdate persists the mode for all git sources.
    PluginAutoUpdateMode autoUpdateMode = PluginAutoUpdateMode::None;
    std::function<void(PluginAutoUpdateMode)> setAutoUpdate;
    // Update every enabled git source at once (the "update all" action).
    std::function<void()> updateAll;

    // Used to derive current toggle state while async discovery refreshes.
    const Config* config = nullptr;
    // Opens the plugin page (about, permissions, settings, Remove).
    std::function<void(std::string id)> onConfigure;
    std::function<void()> openStore;
  };

  // Render the Plugins section into `content` when ctx.selectedSection == "plugins".
  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx);

  // True when the plugin exposes anything the settings editor can show.
  [[nodiscard]] bool pluginHasSettings(const scripting::PluginManifest& manifest);

  void buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale
  );

} // namespace settings
