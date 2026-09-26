#include "wayland/input_control.h"

#include "desktop-unstable-v1-client-protocol.h"

#include <wayland-client.h>

struct InputControlListeners {
  static InputControl& self(void* data) { return *static_cast<InputControl*>(data); }

  static void deviceAdded(void* data, dsk_input_manager_v1* /*manager*/, const char* name, uint32_t kind) {
    InputControl& control = self(data);
    control.m_devices.push_back({.name = name, .kind = static_cast<InputDeviceKind>(kind)});
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void deviceRemoved(void* data, dsk_input_manager_v1* /*manager*/, const char* name) {
    InputControl& control = self(data);
    std::erase_if(control.m_devices, [&](const InputDevice& d) { return d.name == name; });
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void
  setting(void* data, dsk_input_manager_v1* /*manager*/, const char* key, const char* value, uint32_t locked) {
    InputControl& control = self(data);
    control.m_settings.insert_or_assign(key, InputSetting{.value = value, .locked = locked != 0});
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void done(void* data, dsk_input_manager_v1* /*manager*/) {
    InputControl& control = self(data);
    control.m_ready = true;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static void failed(void* data, dsk_input_manager_v1* /*manager*/, const char* /*key*/, const char* reason) {
    InputControl& control = self(data);
    control.m_lastFailure = reason;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static constexpr dsk_input_manager_v1_listener kManager = {
      .device_added = deviceAdded,
      .device_removed = deviceRemoved,
      .setting = setting,
      .done = done,
      .failed = failed,
  };
};

InputControl::InputControl(
    wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange
)
    : m_onChange(std::move(onChange)) {
  if (registry == nullptr || globalName == 0) {
    return;
  }
  m_manager = static_cast<dsk_input_manager_v1*>(
      wl_registry_bind(registry, globalName, &dsk_input_manager_v1_interface, version)
  );
  dsk_input_manager_v1_add_listener(m_manager, &InputControlListeners::kManager, this);
}

InputControl::~InputControl() {
  if (m_manager != nullptr) {
    dsk_input_manager_v1_destroy(m_manager);
  }
}

std::string InputControl::value(std::string_view key) const {
  const auto it = m_settings.find(std::string(key));
  return it != m_settings.end() ? it->second.value : std::string();
}

bool InputControl::locked(std::string_view key) const {
  const auto it = m_settings.find(std::string(key));
  return it != m_settings.end() && it->second.locked;
}

void InputControl::set(const std::string& key, const std::string& value) {
  if (m_manager != nullptr) {
    m_lastFailure.clear();
    dsk_input_manager_v1_set(m_manager, key.c_str(), value.c_str());
  }
}
