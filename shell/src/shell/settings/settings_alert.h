#pragma once

#include "shell/settings/settings_modal_host.h"
#include "ui/controls/button.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace settings {

  struct SettingsAlertButton {
    std::string text;
    ButtonVariant variant = ButtonVariant::Default;
    // Runs after the alert closes. Empty = the button only closes the alert.
    std::function<void()> onClick;
  };

  // Rulebook 9.10 alert: Escape closes without an action; the default button has focus, so Enter runs it.
  struct SettingsAlertRequest {
    std::string title;
    std::string body;
    std::vector<SettingsAlertButton> buttons;
    std::size_t defaultIndex = 0;
    float scale = 1.0F;
  };

  void showSettingsAlert(SettingsModalHost& host, SettingsAlertRequest request);

} // namespace settings
