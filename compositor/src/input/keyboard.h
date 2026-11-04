#pragma once

#include "config/config.h"
#include "input/key_pipeline.h"

#include <string>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct wlr_input_device;
struct wlr_input_method_keyboard_grab_v2;
struct wlr_keyboard;
struct wlr_keyboard_modifiers;

namespace andromeda {

  class Server;

  class Keyboard {
  public:
    Keyboard(Server& server, wlr_input_device* device);
    ~Keyboard();

    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    [[nodiscard]] wlr_keyboard* wlr() const { return m_keyboard; }
    // Virtual keyboards own their keymap and groups; compositor configuration
    // and layout synchronization must not replace or rotate them.
    [[nodiscard]] bool virtualDevice() const { return m_virtual; }
    void applyConfig();
    // Lock the next XKB group, wrapping at the end. False when this physical
    // keyboard has fewer than two layouts.
    bool cycleLayout();
    // Adopt the source keyboard's effective layout when this keymap contains
    // the same named layout, without selecting this keyboard on the seat.
    void syncLayoutFrom(const Keyboard& source);
    // Select a named layout on this physical keyboard without making it the
    // seat's active input device. False when the keymap lacks that layout.
    bool setLayoutByName(std::string_view name);
    // Keycodes whose press a compositor bind consumed, so neither half of the
    // event reaches clients. A focus enter must exclude them: the client never
    // saw the press and will never see the release.
    [[nodiscard]] const std::unordered_set<uint32_t>& consumedKeycodes() const { return m_consumedKeycodes; }
    // Forget consumed keys whose release will never arrive, for example after a
    // VT switch. Otherwise the stale entry swallows a later, unrelated release.
    void forgetConsumedKeycodes() { m_consumedKeycodes.clear(); }
    void prepareInputCapture(bool ending);
    // Runs a key through the pipeline after the bindings stage, as dsk_shell_v1 key and insert_text need. The key is
    // the one in the current layout whose base level (insert_text) or lowest level (key) yields `keysym`; a key above
    // the base level carries its level's modifiers. Returns its evdev keycode, or nullopt when the keymap has no such
    // key or an input-capture session owns the keyboard.
    std::optional<uint32_t> injectKeysym(xkb_keysym_t keysym, uint32_t state, bool baseLevelOnly);
    // Same pipeline entry for a known evdev keycode. A modifier key also updates the seat modifier state.
    void injectKeycode(uint32_t keycode, uint32_t state);

  private:
    void sendSeatModifiers(wlr_keyboard_modifiers modifiers);
    [[nodiscard]] bool inputCaptureOwnsKeyboard() const;
    static void onModifiers(wl_listener* listener, void* data);
    static void onKey(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleModifiers();
    void handleKey(void* data);
    // Pipeline stages 3, 5 and 6; stages 1, 2 and 4 are empty slots.
    KeyResult bindingsStage(KeyEvent& event);
    KeyResult inputMethodStage(KeyEvent& event);
    KeyResult focusedClientStage(KeyEvent& event);
    void handleDestroy();
    void setLayout(xkb_layout_index_t group);
    // Report a real effective-group transition. Initial setup, config reloads,
    // and compositor-driven synchronization update the tracked index directly.
    void notifyLayoutIfChanged();
    // The input-method grab for this keyboard, or null when none applies. Always null while locked, so an IME cannot
    // keylog the lock screen.
    [[nodiscard]] wlr_input_method_keyboard_grab_v2* activeInputMethodGrab() const;
    void armRepeat(const Keybind& bind, uint32_t keycode);
    void cancelRepeat();
    static int onRepeatTimer(void* data);

    // Per-event state shared by handleKey and the bindings stage; valid only during a pipeline run.
    struct KeyContext {
      const xkb_keysym_t* syms;
      int nsyms;
      uint32_t rawSym;
      bool modifierOnly;
      uint32_t modifiers;
    };
    const KeyContext* m_context = nullptr;
    std::optional<Keybind> m_modifierTap;
    KeyPipeline m_pipeline;

    Server* m_server = nullptr;
    wlr_keyboard* m_keyboard = nullptr;
    bool m_virtual = false;
    std::string m_deviceName;
    bool m_syncingLayout = false;
    xkb_layout_index_t m_lastNotifiedLayout = XKB_LAYOUT_INVALID;

    wl_listener m_modifiers{};
    wl_listener m_key{};
    wl_listener m_destroy{};

    wl_event_source* m_repeatTimer = nullptr;
    Keybind m_repeatBind{};
    uint32_t m_repeatKeycode = 0;
    int m_repeatIntervalMs = 0;
    bool m_repeatArmed = false;
    // Keycodes whose press a keybind consumed, so the release is withheld from the focused client too.
    std::unordered_set<uint32_t> m_consumedKeycodes;
  };

} // namespace andromeda
