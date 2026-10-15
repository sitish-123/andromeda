#include "wayland/remote_input_service.h"

#include "core/log.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <unistd.h>
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  constexpr Logger kLog("remote-input");
  // Xkb keycodes start at 8, and 8 itself is never a key.
  constexpr std::uint32_t kFirstKeycode = 9;
  // Codes are recycled past this, since the keymap goes to every focused client.
  constexpr std::size_t kMaxKeysyms = 200;

  std::uint32_t eventTimeMs() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  }

  // One UTF-8 character's code point at text[i], advancing i; U+FFFD for a malformed sequence.
  char32_t nextCodePoint(std::string_view text, std::size_t& i) {
    const auto lead = static_cast<unsigned char>(text[i++]);
    const int extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
    if (extra < 0 || i + static_cast<std::size_t>(extra) > text.size()) {
      return U'�';
    }
    char32_t point = extra == 0 ? lead : lead & (0x3F >> extra);
    for (int n = 0; n < extra; ++n) {
      point = (point << 6) | (static_cast<unsigned char>(text[i++]) & 0x3F);
    }
    return point;
  }

} // namespace

RemoteInputService::RemoteInputService(WaylandConnection& wayland) : m_wayland(wayland) {}

RemoteInputService::~RemoteInputService() {
  if (m_pointer != nullptr) {
    zwlr_virtual_pointer_v1_destroy(m_pointer);
  }
  if (m_keyboard != nullptr) {
    zwp_virtual_keyboard_v1_destroy(m_keyboard);
  }
}

bool RemoteInputService::move(double dx, double dy) {
  if (!ensurePointer()) {
    return false;
  }
  zwlr_virtual_pointer_v1_motion(m_pointer, eventTimeMs(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
  zwlr_virtual_pointer_v1_frame(m_pointer);
  flush();
  return true;
}

bool RemoteInputService::scroll(double dx, double dy) {
  if (!ensurePointer()) {
    return false;
  }
  const std::uint32_t time = eventTimeMs();
  // Continuous, so no axis_stop is owed when the fingers lift.
  zwlr_virtual_pointer_v1_axis_source(m_pointer, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
  if (dy != 0.0) {
    zwlr_virtual_pointer_v1_axis(m_pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
  }
  if (dx != 0.0) {
    zwlr_virtual_pointer_v1_axis(m_pointer, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
  }
  zwlr_virtual_pointer_v1_frame(m_pointer);
  flush();
  return true;
}

bool RemoteInputService::button(std::uint32_t button, bool pressed) {
  if (!ensurePointer()) {
    return false;
  }
  zwlr_virtual_pointer_v1_button(
      m_pointer, eventTimeMs(), button, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
  );
  zwlr_virtual_pointer_v1_frame(m_pointer);
  flush();
  return true;
}

bool RemoteInputService::key(std::uint32_t keysym) {
  if (!ensureKeyboard()) {
    return false;
  }
  const std::uint32_t code = codeFor(keysym);
  if (code == 0) {
    return false;
  }
  const std::uint32_t evdev = code - 8;
  zwp_virtual_keyboard_v1_key(m_keyboard, eventTimeMs(), evdev, WL_KEYBOARD_KEY_STATE_PRESSED);
  zwp_virtual_keyboard_v1_key(m_keyboard, eventTimeMs(), evdev, WL_KEYBOARD_KEY_STATE_RELEASED);
  flush();
  return true;
}

bool RemoteInputService::text(std::string_view utf8) {
  std::size_t i = 0;
  while (i < utf8.size()) {
    const char32_t point = nextCodePoint(utf8, i);
    const std::uint32_t keysym = point == U'\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(point);
    if (keysym == XKB_KEY_NoSymbol || !key(keysym)) {
      return false;
    }
  }
  return true;
}

bool RemoteInputService::ensurePointer() {
  if (m_pointer != nullptr) {
    return true;
  }
  if (m_wayland.virtualPointerManager() == nullptr || m_wayland.seat() == nullptr) {
    kLog.warn("the compositor has no virtual pointer");
    return false;
  }
  m_pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m_wayland.virtualPointerManager(), m_wayland.seat());
  return m_pointer != nullptr;
}

bool RemoteInputService::ensureKeyboard() {
  if (m_keyboard != nullptr) {
    return true;
  }
  if (m_wayland.virtualKeyboardManager() == nullptr || m_wayland.seat() == nullptr) {
    kLog.warn("the compositor has no virtual keyboard");
    return false;
  }
  m_keyboard =
      zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(m_wayland.virtualKeyboardManager(), m_wayland.seat());
  return m_keyboard != nullptr && uploadKeymap();
}

std::uint32_t RemoteInputService::codeFor(std::uint32_t keysym) {
  for (std::size_t i = 0; i < m_keysyms.size(); ++i) {
    if (m_keysyms[i] == keysym) {
      return kFirstKeycode + static_cast<std::uint32_t>(i);
    }
  }
  if (m_keysyms.size() >= kMaxKeysyms) {
    m_keysyms.clear();
  }
  m_keysyms.push_back(keysym);
  if (!uploadKeymap()) {
    return 0;
  }
  return kFirstKeycode + static_cast<std::uint32_t>(m_keysyms.size() - 1);
}

// The keymap the compositor compiles: one key per keysym typed so far, no modifiers.
bool RemoteInputService::uploadKeymap() {
  std::string keycodes;
  std::string symbols;
  for (std::size_t i = 0; i < m_keysyms.size(); ++i) {
    std::array<char, 64> name{};
    if (xkb_keysym_get_name(m_keysyms[i], name.data(), name.size()) <= 0) {
      std::snprintf(name.data(), name.size(), "0x%x", m_keysyms[i]);
    }
    keycodes += std::format("<K{}> = {};\n", i, kFirstKeycode + i);
    symbols += std::format("key <K{}> {{ [ {} ] }};\n", i, name.data());
  }
  const std::string keymap = std::format(
      "xkb_keymap {{\nxkb_keycodes \"link\" {{\nminimum = 8;\nmaximum = {};\n{}}};\n"
      "xkb_types \"link\" {{ include \"complete\" }};\nxkb_compatibility \"link\" {{ include \"complete\" }};\n"
      "xkb_symbols \"link\" {{\n{}}};\n}};\n",
      kFirstKeycode + std::max<std::size_t>(m_keysyms.size(), 1), keycodes, symbols
  );
  char path[] = "/tmp/noctalia-remote-keymap-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    kLog.warn("no file for the remote keymap");
    return false;
  }
  unlink(path);
  const std::size_t size = keymap.size() + 1;
  const bool wrote = write(fd, keymap.c_str(), size) == static_cast<ssize_t>(size);
  if (wrote) {
    zwp_virtual_keyboard_v1_keymap(m_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<std::uint32_t>(size));
  }
  close(fd);
  return wrote;
}

void RemoteInputService::flush() {
  if (wl_display* display = m_wayland.display(); display != nullptr) {
    (void)wl_display_flush(display);
  }
}
