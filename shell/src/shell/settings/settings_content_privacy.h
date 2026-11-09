#pragma once

#include <functional>
#include <string>
#include <vector>

class Flex;

namespace settings {

  // The Password page's fields; they live in the window so a rebuild keeps the typed text.
  struct PasswordDraft {
    std::string current;
    std::string next;
    std::string verify;
    bool busy = false;
    // Green after a change, red after a refusal.
    std::string status;
    bool statusError = false;
  };

  // One App Access row: a portal permission store table and the apps it holds.
  struct AccessKindView {
    std::string table;
    std::string titleKey;
    std::string glyph;
    // Saved screen-sharing tokens: they can only be removed, not allowed again.
    bool tokens = false;
    bool loaded = false;
    struct App {
      std::string id;
      std::string label;
      bool allowed = false;
    };
    std::vector<App> apps;
  };

  struct SettingsPrivacyContext {
    float scale = 1.0F;
    // "privacy:<title>\t<kind>" (kind face, finger, password, access:<table> or rename; rename's title is the face name), else empty.
    std::string detail;
    // False without a system bus: the Sign-in rows show "Not available" and open nothing.
    bool biometricsKnown = false;
    bool facePresent = false;
    bool fingerPresent = false;
    bool fprintdMissing = false;
    std::vector<std::string> faces;
    // fprintd finger ids ("right-index-finger").
    std::vector<std::string> fingers;
    // 0 none, 1 face, 2 finger.
    int enrolling = 0;
    float enrollProgress = 0.0F;
    std::string enrollPrompt;
    // The last add, rename or remove error; empty when it worked.
    std::string status;
    bool faceUnlock = false;
    bool faceOnRequest = true;
    bool fingerprintUnlock = false;
    // pam_gaze in /etc/pam.d/sudo and the helper are installed: the "Admin prompts" switch shows.
    bool faceAdminKnown = false;
    bool faceAdmin = false;
    std::function<void(bool)> setFaceAdmin;
    // The helper is installed: the fingerprint "Admin prompts" switch shows.
    bool fingerAdminKnown = false;
    bool fingerAdmin = false;
    std::function<void(bool)> setFingerAdmin;
    std::function<void(std::string)> openDetail;
    std::function<void()> back;
    std::function<void(std::vector<std::string> path, bool value)> setBool;
    std::function<void()> addFace;
    std::function<void()> addFinger;
    std::function<void()> cancelEnroll;
    std::function<void(std::string name)> removeFace;
    std::function<void(std::string from, std::string to)> renameFace;
    std::function<void(std::string finger)> removeFinger;
    PasswordDraft* password = nullptr;
    // Empty without a session bus: no App Access card.
    std::vector<AccessKindView> access;
    std::function<void(std::string table, std::string app, bool allowed)> setAccess;
    std::function<void()> changePassword;
  };

  // Privacy and Security page (direction A, .agent/DESIGN.md): Sign-in card, then the page's registry rows; or the
  // Face Unlock or Fingerprint page.
  void addSettingsPrivacy(Flex& content, const SettingsPrivacyContext& ctx);

} // namespace settings
