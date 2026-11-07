#include "server/app_menu.h"

#include "gtk-shell-protocol.h"
#include "kde-appmenu-protocol.h"

#include <algorithm>

namespace andromeda {

  namespace {
    constexpr uint32_t kKdeVersion = 1;
    // Version 1 has the dbus properties; later versions hand title-bar gestures to the compositor, which we do not do.
    constexpr uint32_t kGtkVersion = 1;

    std::string str(const char* value) { return value != nullptr ? value : ""; }
  } // namespace

  struct AppMenuRegistry::Entry {
    AppMenuRegistry* registry = nullptr;
    wlr_surface* surface = nullptr;
    wlr_addon addon{};
    std::optional<AppMenuAddress> address;
    std::vector<wl_resource*> resources;
  };

  struct AppMenuCallbacks {
    static const wlr_addon_interface kAddon;
    static const struct org_kde_kwin_appmenu_manager_interface kKdeManager;
    static const struct org_kde_kwin_appmenu_interface kKdeMenu;
    static const struct gtk_shell1_interface kGtkShell;
    static const struct gtk_surface1_interface kGtkSurface;

    static AppMenuRegistry::Entry* entry(wl_resource* resource) {
      return static_cast<AppMenuRegistry::Entry*>(wl_resource_get_user_data(resource));
    }

    static void setAddress(wl_resource* resource, std::optional<AppMenuAddress> address) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state != nullptr && state->address != address) {
        state->address = std::move(address);
        state->registry->changed();
      }
    }

    static void resourceDestroyed(wl_resource* resource) {
      AppMenuRegistry::Entry* state = entry(resource);
      if (state == nullptr) {
        return;
      }
      std::erase(state->resources, resource);
      if (state->resources.empty()) {
        setAddress(resource, std::nullopt);
      }
    }

    static void attach(wl_client* client, wl_resource* parent, const wl_interface* interface, const void* impl,
                       uint32_t id, wl_resource* surfaceResource, AppMenuRegistry* self) {
      wl_resource* resource = wl_resource_create(client, interface, wl_resource_get_version(parent), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      AppMenuRegistry::Entry& state = self->entryFor(wlr_surface_from_resource(surfaceResource));
      state.resources.push_back(resource);
      wl_resource_set_implementation(resource, impl, &state, resourceDestroyed);
    }

    static void bindKde(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource =
          wl_resource_create(client, &org_kde_kwin_appmenu_manager_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kKdeManager, data, nullptr);
    }

    static void bindGtk(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource = wl_resource_create(client, &gtk_shell1_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kGtkShell, data, nullptr);
      // No global menu bar capability: GTK keeps its in-window menu bars.
      gtk_shell1_send_capabilities(resource, 0);
    }
  };

  const wlr_addon_interface AppMenuCallbacks::kAddon = {
      .name = "andromeda_app_menu",
      .destroy =
          [](wlr_addon* addon) {
            AppMenuRegistry::Entry* state = wl_container_of(addon, state, addon);
            state->registry->destroy(state);
          },
  };

  const struct org_kde_kwin_appmenu_manager_interface AppMenuCallbacks::kKdeManager = {
      .create =
          [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface) {
            attach(client, resource, &org_kde_kwin_appmenu_interface, &kKdeMenu, id, surface,
                   static_cast<AppMenuRegistry*>(wl_resource_get_user_data(resource)));
          },
  };

  const struct org_kde_kwin_appmenu_interface AppMenuCallbacks::kKdeMenu = {
      .set_address =
          [](wl_client*, wl_resource* resource, const char* service, const char* path) {
            setAddress(resource, AppMenuAddress{.kind = AppMenuAddress::Kind::DbusMenu, .service = str(service),
                                                .path = str(path), .applicationPath = {}, .windowPath = {}});
          },
      .release = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
  };

  const struct gtk_shell1_interface AppMenuCallbacks::kGtkShell = {
      .get_gtk_surface =
          [](wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface) {
            attach(client, resource, &gtk_surface1_interface, &kGtkSurface, id, surface,
                   static_cast<AppMenuRegistry*>(wl_resource_get_user_data(resource)));
          },
      .set_startup_id = [](wl_client*, wl_resource*, const char*) {},
      .system_bell = [](wl_client*, wl_resource*, wl_resource*) {},
      .notify_launch = nullptr,
  };

  const struct gtk_surface1_interface AppMenuCallbacks::kGtkSurface = {
      .set_dbus_properties =
          [](wl_client*, wl_resource* resource, const char* /*applicationId*/, const char* /*appMenuPath*/,
             const char* menubarPath, const char* windowPath, const char* applicationPath, const char* busName) {
            if (menubarPath == nullptr || busName == nullptr) {
              setAddress(resource, std::nullopt);
              return;
            }
            setAddress(resource, AppMenuAddress{.kind = AppMenuAddress::Kind::Gtk, .service = busName,
                                                .path = menubarPath, .applicationPath = str(applicationPath),
                                                .windowPath = str(windowPath)});
          },
      .set_modal = [](wl_client*, wl_resource*) {},
      .unset_modal = [](wl_client*, wl_resource*) {},
      .present = [](wl_client*, wl_resource*, uint32_t) {},
      .request_focus = nullptr,
      .release = nullptr,
      .titlebar_gesture = nullptr,
      .set_a11y_properties = nullptr,
  };

  AppMenuRegistry::AppMenuRegistry(wl_display* display, std::function<void()> onChange)
      : m_onChange(std::move(onChange)) {
    m_kdeGlobal =
        wl_global_create(display, &org_kde_kwin_appmenu_manager_interface, kKdeVersion, this, AppMenuCallbacks::bindKde);
    m_gtkGlobal = wl_global_create(display, &gtk_shell1_interface, kGtkVersion, this, AppMenuCallbacks::bindGtk);
  }

  AppMenuRegistry::~AppMenuRegistry() {
    while (!m_entries.empty()) {
      destroy(m_entries.back().get());
    }
    if (m_kdeGlobal != nullptr) {
      wl_global_destroy(m_kdeGlobal);
    }
    if (m_gtkGlobal != nullptr) {
      wl_global_destroy(m_gtkGlobal);
    }
  }

  AppMenuRegistry::Entry& AppMenuRegistry::entryFor(wlr_surface* surface) {
    if (wlr_addon* addon = wlr_addon_find(&surface->addons, this, &AppMenuCallbacks::kAddon)) {
      Entry* existing = wl_container_of(addon, existing, addon);
      return *existing;
    }
    auto owned = std::make_unique<Entry>();
    owned->registry = this;
    owned->surface = surface;
    wlr_addon_init(&owned->addon, &surface->addons, this, &AppMenuCallbacks::kAddon);
    m_entries.push_back(std::move(owned));
    return *m_entries.back();
  }

  // The surface is gone: its protocol objects stay inert until the client destroys them.
  void AppMenuRegistry::destroy(Entry* entry) {
    wlr_addon_finish(&entry->addon);
    for (wl_resource* resource : entry->resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    const bool hadAddress = entry->address.has_value();
    std::erase_if(m_entries, [&](const auto& owned) { return owned.get() == entry; });
    if (hadAddress) {
      changed();
    }
  }

  void AppMenuRegistry::changed() {
    if (m_onChange) {
      m_onChange();
    }
  }

  std::optional<AppMenuAddress> AppMenuRegistry::addressFor(const wlr_surface* surface) const {
    const auto found = std::ranges::find_if(m_entries, [&](const auto& owned) { return owned->surface == surface; });
    return found != m_entries.end() ? (*found)->address : std::nullopt;
  }

} // namespace andromeda
