#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

class WaylandConnection;
struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_v1;

// A phone's pointer and keyboard on this desktop, through the compositor's virtual pointer and virtual keyboard. Text
// goes through a keymap of its own that grows by each new character, so any Unicode text types, whatever the
// session's layout.
class RemoteInputService {
public:
  explicit RemoteInputService(WaylandConnection& wayland);
  ~RemoteInputService();

  RemoteInputService(const RemoteInputService&) = delete;
  RemoteInputService& operator=(const RemoteInputService&) = delete;

  // Relative motion in logical pixels.
  bool move(double dx, double dy);
  // A finger scroll in logical pixels; positive is down and right.
  bool scroll(double dx, double dy);
  // button is a linux/input-event-codes.h code such as BTN_LEFT.
  bool button(std::uint32_t button, bool pressed);
  // An xkb keysym, pressed and released.
  bool key(std::uint32_t keysym);
  bool text(std::string_view utf8);

private:
  [[nodiscard]] bool ensurePointer();
  [[nodiscard]] bool ensureKeyboard();
  // The keymap's code for keysym, adding it and uploading the grown keymap when it is new.
  [[nodiscard]] std::uint32_t codeFor(std::uint32_t keysym);
  [[nodiscard]] bool uploadKeymap();
  void flush();

  WaylandConnection& m_wayland;
  zwlr_virtual_pointer_v1* m_pointer = nullptr;
  zwp_virtual_keyboard_v1* m_keyboard = nullptr;
  std::vector<std::uint32_t> m_keysyms;
};
