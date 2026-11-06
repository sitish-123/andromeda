#pragma once

#include <string>

class Flex;
class SettingsControl;

namespace settings {

  struct SettingsWorkspacesContext {
    float scale = 1.0F;
    SettingsControl* compositor = nullptr;
    std::string detail;
  };

  // Windows › Per-Workspace Arrangement detail (opened from the Workspaces card): one arrangement picker per workspace
  // slot the compositor reports ("workspace.<N>.layout.mode"; empty = the page's Window Arrangement).
  void addSettingsWorkspaces(Flex& content, const SettingsWorkspacesContext& ctx);

} // namespace settings
