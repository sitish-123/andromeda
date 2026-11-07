#pragma once

#include <string>

namespace password_change {

  struct Result {
    bool ok = false;
    // passwd rejected the current password.
    bool wrongCurrent = false;
    // The account password changed, but the login keyring kept the old one.
    bool keyringFailed = false;
    // passwd's own reason when it refused the new password ("BAD PASSWORD: it is too short").
    std::string message;
  };

  // Changes the current user's password through setuid passwd on a pty, then the gnome-keyring login keyring
  // password. Blocks for seconds (pam_faillock delays a wrong password; a stalled PAM stack up to 4 prompts x 15 s):
  // never call it on the main thread.
  // Both strings are cleared before it returns.
  [[nodiscard]] Result change(std::string& current, std::string& next);

} // namespace password_change
