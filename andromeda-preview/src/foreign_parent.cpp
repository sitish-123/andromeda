#include "foreign_parent.h"

#include "xdg-foreign-unstable-v2-client-protocol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <wayland-client.h>

namespace preview {

  namespace {
    void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t) {
      if (std::strcmp(interface, zxdg_importer_v2_interface.name) == 0) {
        *static_cast<zxdg_importer_v2**>(data) =
            static_cast<zxdg_importer_v2*>(wl_registry_bind(registry, name, &zxdg_importer_v2_interface, 1));
      }
    }
    void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
    constexpr wl_registry_listener kRegistryListener{.global = onGlobal, .global_remove = onGlobalRemove};

    void onDestroyed(void* data, zxdg_imported_v2* imported) {
      auto** slot = static_cast<zxdg_imported_v2**>(data);
      if (*slot == imported) {
        *slot = nullptr;
      }
      zxdg_imported_v2_destroy(imported);
    }
    constexpr zxdg_imported_v2_listener kImportedListener{.destroyed = onDestroyed};

    constexpr std::string_view kWaylandPrefix = "wayland:";
  } // namespace

  ForeignParent::ForeignParent(wl_display* display) {
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &m_importer);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
  }

  ForeignParent::~ForeignParent() {
    clear();
    if (m_importer != nullptr) {
      zxdg_importer_v2_destroy(m_importer);
    }
  }

  void ForeignParent::setParent(wl_surface* child, std::string_view handle) {
    clear();
    if (m_importer == nullptr || child == nullptr || !handle.starts_with(kWaylandPrefix)) {
      return;
    }
    const std::string exported(handle.substr(kWaylandPrefix.size()));
    if (exported.empty()) {
      return;
    }
    m_imported = zxdg_importer_v2_import_toplevel(m_importer, exported.c_str());
    zxdg_imported_v2_add_listener(m_imported, &kImportedListener, &m_imported);
    zxdg_imported_v2_set_parent_of(m_imported, child);
  }

  void ForeignParent::clear() {
    if (m_imported != nullptr) {
      zxdg_imported_v2_destroy(m_imported);
      m_imported = nullptr;
    }
  }

} // namespace preview
