#include "wayland/settings_control.h"

#include "desktop-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <utility>

struct SettingsControlListeners {
  static SettingsControl& self(void* data) { return *static_cast<SettingsControl*>(data); }

  static void deviceAdded(void* data, dsk_settings_manager_v1* /*manager*/, const char* name, uint32_t kind) {
    SettingsControl& control = self(data);
    control.m_devices.push_back({.name = name, .kind = static_cast<InputDeviceKind>(kind)});
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void deviceRemoved(void* data, dsk_settings_manager_v1* /*manager*/, const char* name) {
    SettingsControl& control = self(data);
    std::erase_if(control.m_devices, [&](const InputDevice& d) { return d.name == name; });
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void
  setting(void* data, dsk_settings_manager_v1* /*manager*/, const char* key, const char* value, uint32_t customized) {
    SettingsControl& control = self(data);
    // Settings arrive in batches closed by done, which notifies once.
    control.m_settings.insert_or_assign(key, CompositorSetting{.value = value, .customized = customized != 0});
  }

  static void
  keybind(void* data, dsk_settings_manager_v1* /*manager*/, const char* chord, const char* action, uint32_t customized) {
    SettingsControl& control = self(data);
    // Each batch lists every keybind, so the first one of a batch starts the list over.
    if (!std::exchange(control.m_keybindBatch, true)) {
      control.m_keybinds.clear();
    }
    control.m_keybinds.push_back({.chord = chord, .action = action, .customized = customized != 0});
  }

  static void actionSpec(
      void* data, dsk_settings_manager_v1* /*manager*/, const char* name, const char* param, const char* summary
  ) {
    self(data).m_actions.push_back({.name = name, .param = param, .summary = summary});
  }

  static void chordCaptured(void* data, dsk_settings_manager_v1* /*manager*/, const char* chord) {
    if (auto onCaptured = std::exchange(self(data).m_onCaptured, nullptr)) {
      onCaptured(chord);
    }
  }

  static void done(void* data, dsk_settings_manager_v1* /*manager*/) {
    SettingsControl& control = self(data);
    control.m_keybindBatch = false;
    control.m_ready = true;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static void failed(void* data, dsk_settings_manager_v1* /*manager*/, const char* /*key*/, const char* reason) {
    SettingsControl& control = self(data);
    control.m_lastFailure = reason;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static constexpr dsk_settings_manager_v1_listener kManager = {
      .device_added = deviceAdded,
      .device_removed = deviceRemoved,
      .setting = setting,
      .keybind = keybind,
      .action_spec = actionSpec,
      .chord_captured = chordCaptured,
      .done = done,
      .failed = failed,
  };
};

SettingsControl::SettingsControl(
    wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange
)
    : m_onChange(std::move(onChange)) {
  if (registry == nullptr || globalName == 0) {
    return;
  }
  m_manager = static_cast<dsk_settings_manager_v1*>(
      wl_registry_bind(registry, globalName, &dsk_settings_manager_v1_interface, version)
  );
  dsk_settings_manager_v1_add_listener(m_manager, &SettingsControlListeners::kManager, this);
}

SettingsControl::~SettingsControl() {
  if (m_manager != nullptr) {
    dsk_settings_manager_v1_destroy(m_manager);
  }
}

std::string SettingsControl::value(std::string_view key) const {
  const auto it = m_settings.find(std::string(key));
  return it != m_settings.end() ? it->second.value : std::string();
}

bool SettingsControl::customized(std::string_view key) const {
  const auto it = m_settings.find(std::string(key));
  return it != m_settings.end() && it->second.customized;
}

void SettingsControl::set(const std::string& key, const std::string& value) {
  if (m_manager != nullptr) {
    m_lastFailure.clear();
    dsk_settings_manager_v1_set(m_manager, key.c_str(), value.c_str());
  }
}

void SettingsControl::bind(const std::string& chord, const std::string& action) {
  if (m_manager != nullptr) {
    m_lastFailure.clear();
    dsk_settings_manager_v1_bind(m_manager, chord.c_str(), action.c_str());
  }
}

void SettingsControl::captureChord(std::function<void(std::string)> onCaptured) {
  if (m_manager != nullptr) {
    m_onCaptured = std::move(onCaptured);
    dsk_settings_manager_v1_capture_chord(m_manager);
  }
}

void SettingsControl::cancelCapture() {
  if (m_manager != nullptr && m_onCaptured) {
    m_onCaptured = nullptr;
    dsk_settings_manager_v1_cancel_capture(m_manager);
  }
}
