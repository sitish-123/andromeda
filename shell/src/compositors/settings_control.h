#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct dsk_settings_manager_v1;
struct wl_registry;

enum class InputDeviceKind : std::uint8_t { Keyboard = 1, Mouse = 2, Touchpad = 3, Touch = 4, Tablet = 5 };

struct InputDevice {
  std::string name;
  InputDeviceKind kind{};
};

struct CompositorSetting {
  std::string value;
  bool customized = false; // the compositor's settings file holds this key
};

struct CompositorKeybind {
  std::string chord;  // as the config file writes it, "Mod+Shift+q"
  std::string action; // "window-close", "spawn:foot"; "none" for a chord the settings file unbinds
  bool customized = false;
};

struct CompositorAction {
  std::string name;
  std::string param; // empty for an action without an argument, else a hint such as "<cmd>"
  std::string summary;
};

// Client of dsk_settings_manager_v1: the compositor settings Settings can change, keyed by their config.toml name
// ("input.touchpad.tap", "appearance.border_width"), and the input devices. Binds only while Settings is open.
class SettingsControl {
public:
  using ChangeCallback = std::function<void()>;

  SettingsControl(wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange);
  ~SettingsControl();

  SettingsControl(const SettingsControl&) = delete;
  SettingsControl& operator=(const SettingsControl&) = delete;

  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  [[nodiscard]] const std::vector<InputDevice>& devices() const noexcept { return m_devices; }
  [[nodiscard]] const std::unordered_map<std::string, CompositorSetting>& settings() const noexcept {
    return m_settings;
  }
  // Value for `key`, empty when unset or unknown.
  [[nodiscard]] std::string value(std::string_view key) const;
  [[nodiscard]] bool customized(std::string_view key) const;
  // Reason the compositor gave for the last rejected `set`, empty when none.
  [[nodiscard]] const std::string& lastFailure() const noexcept { return m_lastFailure; }

  // Empty value clears the setting back to its device or built-in default.
  void set(const std::string& key, const std::string& value);

  [[nodiscard]] const std::vector<CompositorKeybind>& keybinds() const noexcept { return m_keybinds; }
  [[nodiscard]] const std::vector<CompositorAction>& actions() const noexcept { return m_actions; }
  // Empty action removes the settings file's entry for `chord`; "none" unbinds it.
  void bind(const std::string& chord, const std::string& action);
  // The compositor reports the next chord pressed, before any keybind sees it; empty when cancelled.
  void captureChord(std::function<void(std::string)> onCaptured);
  void cancelCapture();
  [[nodiscard]] bool capturing() const noexcept { return static_cast<bool>(m_onCaptured); }

private:
  friend struct SettingsControlListeners;

  dsk_settings_manager_v1* m_manager = nullptr;
  std::vector<InputDevice> m_devices;
  std::unordered_map<std::string, CompositorSetting> m_settings;
  std::vector<CompositorKeybind> m_keybinds;
  std::vector<CompositorAction> m_actions;
  std::function<void(std::string)> m_onCaptured;
  std::string m_lastFailure;
  bool m_keybindBatch = false;
  bool m_ready = false;
  ChangeCallback m_onChange;
};
