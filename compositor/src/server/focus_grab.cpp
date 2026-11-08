#include "server/focus_grab.h"

#include "hyprland-focus-grab-v1-protocol.h"

#include <algorithm>

namespace andromeda {

  namespace {
    constexpr uint32_t kVersion = 1;
  } // namespace

  // A surface in a grab's whitelist: pending until commit, committed after. Its destroy removes it from both.
  struct FocusGrabManager::Member {
    FocusGrabManager::Grab* grab = nullptr;
    wlr_surface* surface = nullptr;
    bool pending = false;
    bool committed = false;
    wl_listener destroy{};
  };

  struct FocusGrabManager::Grab {
    FocusGrabManager* manager = nullptr;
    wl_resource* resource = nullptr;
    std::vector<std::unique_ptr<Member>> members;

    [[nodiscard]] bool active() const {
      return std::ranges::any_of(members, [](const auto& member) { return member->committed; });
    }
    [[nodiscard]] bool contains(wlr_surface* surface) const {
      return std::ranges::any_of(members, [surface](const auto& m) { return m->committed && m->surface == surface; });
    }
    void prune() {
      std::erase_if(members, [](const std::unique_ptr<Member>& member) {
        if (member->pending || member->committed) {
          return false;
        }
        wl_list_remove(&member->destroy.link);
        return true;
      });
    }
  };

  struct FocusGrabCallbacks {
    static const struct hyprland_focus_grab_manager_v1_interface kManager;
    static const struct hyprland_focus_grab_v1_interface kGrab;

    static FocusGrabManager::Grab* grab(wl_resource* resource) {
      return static_cast<FocusGrabManager::Grab*>(wl_resource_get_user_data(resource));
    }

    static void grabDestroyed(wl_resource* resource) {
      FocusGrabManager::Grab* state = grab(resource);
      if (state == nullptr) {
        return;
      }
      for (const auto& member : state->members) {
        wl_list_remove(&member->destroy.link);
      }
      if (state->manager != nullptr) {
        std::erase(state->manager->m_grabs, state);
      }
      delete state;
    }

    static void surfaceDestroyed(wl_listener* listener, void*) {
      FocusGrabManager::Member* member = wl_container_of(listener, member, destroy);
      FocusGrabManager::Grab* state = member->grab;
      member->pending = false;
      member->committed = false;
      state->prune();
    }

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
      wl_resource* resource =
          wl_resource_create(client, &hyprland_focus_grab_manager_v1_interface, static_cast<int>(version), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kManager, data, nullptr);
    }
  };

  const struct hyprland_focus_grab_manager_v1_interface FocusGrabCallbacks::kManager = {
      .create_grab =
          [](wl_client* client, wl_resource* resource, uint32_t id) {
            auto* manager = static_cast<FocusGrabManager*>(wl_resource_get_user_data(resource));
            wl_resource* grabResource =
                wl_resource_create(client, &hyprland_focus_grab_v1_interface, wl_resource_get_version(resource), id);
            if (grabResource == nullptr) {
              wl_client_post_no_memory(client);
              return;
            }
            auto* state = new FocusGrabManager::Grab{.manager = manager, .resource = grabResource, .members = {}};
            wl_resource_set_implementation(grabResource, &kGrab, state, grabDestroyed);
            if (manager != nullptr) {
              manager->m_grabs.push_back(state);
            }
          },
      .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
  };

  const struct hyprland_focus_grab_v1_interface FocusGrabCallbacks::kGrab = {
      .add_surface =
          [](wl_client*, wl_resource* resource, wl_resource* surfaceResource) {
            FocusGrabManager::Grab* state = grab(resource);
            wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
            for (const auto& member : state->members) {
              if (member->surface == surface) {
                member->pending = true;
                return;
              }
            }
            auto member = std::make_unique<FocusGrabManager::Member>();
            member->grab = state;
            member->surface = surface;
            member->pending = true;
            member->destroy.notify = surfaceDestroyed;
            wl_signal_add(&surface->events.destroy, &member->destroy);
            state->members.push_back(std::move(member));
          },
      .remove_surface =
          [](wl_client*, wl_resource* resource, wl_resource* surfaceResource) {
            FocusGrabManager::Grab* state = grab(resource);
            wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
            for (const auto& member : state->members) {
              if (member->surface == surface) {
                member->pending = false;
              }
            }
          },
      .commit =
          [](wl_client*, wl_resource* resource) {
            FocusGrabManager::Grab* state = grab(resource);
            const bool wasActive = state->active();
            for (const auto& member : state->members) {
              member->committed = member->pending;
            }
            state->prune();
            if (!wasActive && state->active() && state->manager != nullptr) {
              state->manager->activated(state);
            }
          },
      .destroy = [](wl_client*, wl_resource* resource) { wl_resource_destroy(resource); },
  };

  FocusGrabManager::FocusGrabManager(wl_display* display) {
    m_global = wl_global_create(display, &hyprland_focus_grab_manager_v1_interface, static_cast<int>(kVersion), this,
                                FocusGrabCallbacks::bind);
  }

  FocusGrabManager::~FocusGrabManager() {
    for (Grab* grab : m_grabs) {
      grab->manager = nullptr;
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void FocusGrabManager::activated(Grab* grab) {
    // One panel grab at a time: the newest wins, older ones are cleared.
    const std::vector<Grab*> grabs = m_grabs;
    for (Grab* other : grabs) {
      if (other != grab && other->active()) {
        clear(*other);
      }
    }
  }

  void FocusGrabManager::clear(Grab& grab) {
    for (const auto& member : grab.members) {
      member->pending = false;
      member->committed = false;
    }
    grab.prune();
    hyprland_focus_grab_v1_send_cleared(grab.resource);
  }

  bool FocusGrabManager::active() const {
    return std::ranges::any_of(m_grabs, [](const Grab* grab) { return grab->active(); });
  }

  void FocusGrabManager::press(wlr_surface* surface) {
    // Subsurfaces count as their parent; the client adds its popups itself.
    wlr_surface* root = surface != nullptr ? wlr_surface_get_root_surface(surface) : nullptr;
    const std::vector<Grab*> grabs = m_grabs;
    for (Grab* grab : grabs) {
      if (grab->active() && (root == nullptr || (!grab->contains(root) && !grab->contains(surface)))) {
        clear(*grab);
      }
    }
  }

} // namespace andromeda
