#include "wayland/mirror_control.h"

#include "desktop-unstable-v1-client-protocol.h"

#include <wayland-client.h>

struct MirrorControlListeners {
  static MirrorControl& self(void* data) { return *static_cast<MirrorControl*>(data); }

  static void mirror(void* data, dsk_output_manager_v1* /*manager*/, const char* target, const char* source) {
    MirrorControl& control = self(data);
    if (source[0] == '\0') {
      control.m_mirrors.erase(target);
    } else {
      control.m_mirrors.insert_or_assign(target, source);
    }
    if (control.m_ready && control.m_onChange) {
      control.m_onChange();
    }
  }

  static void done(void* data, dsk_output_manager_v1* /*manager*/) {
    MirrorControl& control = self(data);
    control.m_ready = true;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static void failed(void* data, dsk_output_manager_v1* /*manager*/, const char* /*target*/, const char* reason) {
    MirrorControl& control = self(data);
    control.m_lastFailure = reason;
    if (control.m_onChange) {
      control.m_onChange();
    }
  }

  static constexpr dsk_output_manager_v1_listener kManager = {.mirror = mirror, .done = done, .failed = failed};
};

MirrorControl::MirrorControl(
    wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange
)
    : m_onChange(std::move(onChange)) {
  if (registry == nullptr || globalName == 0) {
    return;
  }
  m_manager = static_cast<dsk_output_manager_v1*>(
      wl_registry_bind(registry, globalName, &dsk_output_manager_v1_interface, version)
  );
  dsk_output_manager_v1_add_listener(m_manager, &MirrorControlListeners::kManager, this);
}

MirrorControl::~MirrorControl() {
  if (m_manager != nullptr) {
    dsk_output_manager_v1_destroy(m_manager);
  }
}

void MirrorControl::setMirror(const std::string& target, const std::string& source) {
  if (m_manager != nullptr) {
    m_lastFailure.clear();
    dsk_output_manager_v1_set_mirror(m_manager, target.c_str(), source.c_str());
  }
}

void MirrorControl::clearMirror(const std::string& target) {
  if (m_manager != nullptr) {
    m_lastFailure.clear();
    dsk_output_manager_v1_clear_mirror(m_manager, target.c_str());
  }
}
