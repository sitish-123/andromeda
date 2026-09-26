#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct dsk_input_manager_v1;
struct wl_registry;

enum class InputDeviceKind : std::uint8_t { Keyboard = 1, Mouse = 2, Touchpad = 3, Touch = 4, Tablet = 5 };

struct InputDevice {
  std::string name;
  InputDeviceKind kind{};
};

struct InputSetting {
  std::string value;
  bool locked = false; // the user's own config.toml sets this key
};

// Client of dsk_input_manager_v1: the compositor's 36 input settings, keyed by their config.toml
// name ("input.touchpad.tap"). Binds only while the Input settings page is open, like
// MirrorControl does for the Displays page.
class InputControl {
public:
  using ChangeCallback = std::function<void()>;

  InputControl(wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange);
  ~InputControl();

  InputControl(const InputControl&) = delete;
  InputControl& operator=(const InputControl&) = delete;

  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  [[nodiscard]] const std::vector<InputDevice>& devices() const noexcept { return m_devices; }
  [[nodiscard]] const std::unordered_map<std::string, InputSetting>& settings() const noexcept { return m_settings; }
  // Value for `key`, empty when unset or unknown.
  [[nodiscard]] std::string value(std::string_view key) const;
  [[nodiscard]] bool locked(std::string_view key) const;
  // Reason the compositor gave for the last rejected `set`, empty when none.
  [[nodiscard]] const std::string& lastFailure() const noexcept { return m_lastFailure; }

  // Empty value clears the setting back to its device or built-in default.
  void set(const std::string& key, const std::string& value);

private:
  friend struct InputControlListeners;

  dsk_input_manager_v1* m_manager = nullptr;
  std::vector<InputDevice> m_devices;
  std::unordered_map<std::string, InputSetting> m_settings;
  std::string m_lastFailure;
  bool m_ready = false;
  ChangeCallback m_onChange;
};
