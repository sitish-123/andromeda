#include "app/foreign_export.h"

#include "xdg-foreign-unstable-v2-client-protocol.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <wayland-client.h>

namespace desk {

  namespace {
    void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t) {
      if (std::strcmp(interface, zxdg_exporter_v2_interface.name) == 0) {
        *static_cast<zxdg_exporter_v2**>(data) =
            static_cast<zxdg_exporter_v2*>(wl_registry_bind(registry, name, &zxdg_exporter_v2_interface, 1));
      }
    }
    void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
    constexpr wl_registry_listener kRegistryListener{.global = onGlobal, .global_remove = onGlobalRemove};

    void onExportedHandle(void* data, zxdg_exported_v2*, const char* handle) {
      static_cast<ForeignExport*>(data)->onHandle(handle);
    }
    constexpr zxdg_exported_v2_listener kExportedListener{.handle = onExportedHandle};
  } // namespace

  ForeignExport::ForeignExport(wl_display* display) {
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &m_exporter);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
  }

  ForeignExport::~ForeignExport() {
    if (m_exported != nullptr) {
      zxdg_exported_v2_destroy(m_exported);
    }
    if (m_exporter != nullptr) {
      zxdg_exporter_v2_destroy(m_exporter);
    }
  }

  void ForeignExport::handle(wl_surface* surface, std::function<void(const std::string&)> ready) {
    if (m_exporter == nullptr || surface == nullptr) {
      ready({});
      return;
    }
    if (surface == m_surface && !m_handle.empty()) {
      ready(m_handle);
      return;
    }
    if (surface != m_surface) {
      if (m_exported != nullptr) {
        zxdg_exported_v2_destroy(m_exported);
      }
      m_surface = surface;
      m_handle.clear();
      m_exported = zxdg_exporter_v2_export_toplevel(m_exporter, surface);
      zxdg_exported_v2_add_listener(m_exported, &kExportedListener, this);
    }
    m_ready = std::move(ready);
  }

  void ForeignExport::onHandle(const char* handle) {
    m_handle = std::string("wayland:") + handle;
    if (auto ready = std::exchange(m_ready, {})) {
      ready(m_handle);
    }
  }

} // namespace desk
