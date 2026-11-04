#include "server/desktop_shell.h"

#include "core/log.h"
#include "desktop-unstable-v1-protocol.h"
#include "input/keyboard.h"
#include "input/text_input.h"
#include "server/server.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace andromeda {

  namespace {

    constexpr Logger kLog("desktop-shell");
    constexpr uint32_t kVersion = 3;
    constexpr size_t kMaxAppActivity = 256;

    DesktopShell* shellFrom(wl_resource* resource) {
      return static_cast<DesktopShell*>(wl_resource_get_user_data(resource));
    }

  } // namespace

  DesktopShell::DesktopShell(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_shell_v1_interface, kVersion, this, bind);
  }

  DesktopShell::~DesktopShell() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopShell::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopShell*>(data);
    wl_resource* resource = wl_resource_create(client, &dsk_shell_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    self->sendLockKeys(resource);
    self->sendTextInputState(resource);
    // A shell that binds after a restart into the lock (Server::enterRestartLock) still must start its lock screen.
    if (self->m_relocked && version >= DSK_SHELL_V1_RELOCK_SINCE_VERSION) {
      dsk_shell_v1_send_relock(resource);
    }
  }

  void DesktopShell::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopShell::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopShell* self = shellFrom(resource)) {
      std::erase(self->m_resources, resource);
      std::erase_if(self->m_appActivity, [resource](const auto& entry) { return entry.second.owner == resource; });
      self->releaseKeys();
    }
  }

  bool DesktopShell::sendAction(std::string_view command) {
    const std::string line(command);
    for (wl_resource* resource : m_resources) {
      dsk_shell_v1_send_action(resource, line.c_str());
    }
    return !m_resources.empty();
  }

  bool DesktopShell::sendBell(std::string_view appId) {
    const std::string id(appId);
    bool sent = false;
    for (wl_resource* resource : m_resources) {
      if (wl_resource_get_version(resource) >= DSK_SHELL_V1_BELL_SINCE_VERSION) {
        dsk_shell_v1_send_bell(resource, id.c_str());
        sent = true;
      }
    }
    return sent;
  }

  void DesktopShell::setLockKeys(bool capsLock, bool numLock, bool scrollLock) {
    if (capsLock == m_capsLock && numLock == m_numLock && scrollLock == m_scrollLock) {
      return;
    }
    m_capsLock = capsLock;
    m_numLock = numLock;
    m_scrollLock = scrollLock;
    for (wl_resource* resource : m_resources) {
      sendLockKeys(resource);
    }
  }

  void DesktopShell::setTextInputState(
      bool enabled, uint32_t purpose, uint32_t hint, int x, int y, int width, int height
  ) {
    const TextInputState state{enabled, purpose, hint, x, y, width, height};
    if (state == m_textInput) {
      return;
    }
    m_textInput = state;
    for (wl_resource* resource : m_resources) {
      sendTextInputState(resource);
    }
  }

  void DesktopShell::sendTextInputState(wl_resource* resource) const {
    if (wl_resource_get_version(resource) < DSK_SHELL_V1_TEXT_INPUT_STATE_SINCE_VERSION) {
      return;
    }
    dsk_shell_v1_send_text_input_state(
        resource,
        m_textInput.enabled ? 1 : 0,
        m_textInput.purpose,
        m_textInput.hint,
        m_textInput.x,
        m_textInput.y,
        m_textInput.width,
        m_textInput.height
    );
  }

  void DesktopShell::sendRelock() {
    if (std::exchange(m_relocked, true)) {
      return;
    }
    for (wl_resource* resource : m_resources) {
      if (wl_resource_get_version(resource) >= DSK_SHELL_V1_RELOCK_SINCE_VERSION) {
        dsk_shell_v1_send_relock(resource);
      }
    }
  }

  void DesktopShell::sendRecovered() {
    if (!std::exchange(m_relocked, false)) {
      return;
    }
    for (wl_resource* resource : m_resources) {
      if (wl_resource_get_version(resource) >= DSK_SHELL_V1_RECOVERED_SINCE_VERSION) {
        dsk_shell_v1_send_recovered(resource);
      }
    }
  }

  std::optional<uint32_t> DesktopShell::appActivity(uint32_t pid) const {
    const auto it = m_appActivity.find(pid);
    return it == m_appActivity.end() ? std::nullopt : std::optional<uint32_t>(it->second.flags);
  }

  void DesktopShell::handleAppActivity(wl_client*, wl_resource* resource, uint32_t pid, uint32_t flags) {
    DesktopShell* self = shellFrom(resource);
    if (self == nullptr) {
      return;
    }
    if (flags == 0) {
      self->m_appActivity.erase(pid);
      return;
    }
    std::error_code ec;
    if (!std::filesystem::exists("/proc/" + std::to_string(pid), ec)) {
      kLog.debug("app activity for pid {} dropped: no such process", pid);
      return;
    }
    if (self->m_appActivity.size() >= kMaxAppActivity && !self->m_appActivity.contains(pid)) {
      kLog.warn("app activity for pid {} dropped: {} pids stored", pid, kMaxAppActivity);
      return;
    }
    self->m_appActivity[pid] = {flags, resource};
    kLog.debug("app activity pid={} flags={}", pid, flags);
  }

  void DesktopShell::handleKey(wl_client*, wl_resource* resource, uint32_t keysym, uint32_t state) {
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED && state != WL_KEYBOARD_KEY_STATE_RELEASED) {
      wl_resource_post_error(resource, DSK_SHELL_V1_ERROR_INVALID_KEY_STATE, "invalid key state %u", state);
      return;
    }
    DesktopShell* self = shellFrom(resource);
    if (self == nullptr) {
      return;
    }
    if (self->m_server.sessionLocked()) {
      static bool logged = false;
      if (!std::exchange(logged, true)) {
        kLog.warn("key dropped: the session is locked");
      }
      return;
    }
    Keyboard* keyboard = self->m_server.syntheticKeyboard();
    if (keyboard == nullptr) {
      kLog.debug("key {:#x} dropped: no keyboard", keysym);
      return;
    }
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
      const auto pressed = self->m_pressedKeys.find(keysym);
      if (pressed != self->m_pressedKeys.end()) {
        keyboard->injectKeycode(pressed->second, state);
        self->m_pressedKeys.erase(pressed);
      }
      return;
    }
    if (self->m_pressedKeys.contains(keysym)) {
      return;
    }
    if (const std::optional<uint32_t> keycode = keyboard->injectKeysym(keysym, state, false)) {
      self->m_pressedKeys[keysym] = *keycode;
    } else {
      kLog.debug("key {:#x} dropped: no key for it or the keyboard is captured", keysym);
    }
  }

  void DesktopShell::releaseKeys() {
    Keyboard* keyboard = m_server.syntheticKeyboard();
    for (const auto& [keysym, keycode] : m_pressedKeys) {
      if (keyboard != nullptr) {
        keyboard->injectKeycode(keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
      }
    }
    m_pressedKeys.clear();
  }

  void DesktopShell::handleInsertText(wl_client*, wl_resource* resource, const char* text) {
    DesktopShell* self = shellFrom(resource);
    if (self == nullptr) {
      return;
    }
    if (self->m_server.sessionLocked()) {
      static bool logged = false;
      if (!std::exchange(logged, true)) {
        kLog.warn("insert_text dropped: the session is locked");
      }
      return;
    }
    if (self->m_server.inputMethodRelay()->commitText(text)) {
      return;
    }
    Keyboard* keyboard = self->m_server.syntheticKeyboard();
    if (keyboard == nullptr) {
      kLog.debug("insert_text dropped: no text input and no keyboard");
      return;
    }
    // No text input is enabled, so type the text as key presses; characters the keymap lacks at its base level drop.
    const std::string_view line(text);
    for (size_t i = 0; i < line.size();) {
      const auto lead = static_cast<unsigned char>(line[i]);
      const size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
      char32_t codepoint = length == 1 ? lead : lead & (0xFFU >> (length + 1));
      for (size_t j = 1; j < length && i + j < line.size(); ++j) {
        codepoint = (codepoint << 6) | (static_cast<unsigned char>(line[i + j]) & 0x3FU);
      }
      i += length;
      const xkb_keysym_t sym = codepoint == U'\n' ? XKB_KEY_Return
          : codepoint == U'\t'                    ? XKB_KEY_Tab
                                                   : xkb_utf32_to_keysym(codepoint);
      const std::optional<uint32_t> keycode =
          sym == XKB_KEY_NoSymbol ? std::nullopt : keyboard->injectKeysym(sym, WL_KEYBOARD_KEY_STATE_PRESSED, true);
      if (!keycode) {
        kLog.debug("insert_text dropped U+{:04X}: not in the keymap", static_cast<uint32_t>(codepoint));
        continue;
      }
      keyboard->injectKeycode(*keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
  }

  void DesktopShell::sendLockKeys(wl_resource* resource) const {
    dsk_shell_v1_send_lock_keys(resource, m_capsLock ? 1 : 0, m_numLock ? 1 : 0, m_scrollLock ? 1 : 0);
  }

  const struct dsk_shell_v1_interface DesktopShell::kImplementation = {
      .destroy = DesktopShell::handleDestroy,
      .app_activity = DesktopShell::handleAppActivity,
      .insert_text = DesktopShell::handleInsertText,
      .key = DesktopShell::handleKey,
  };

} // namespace andromeda
