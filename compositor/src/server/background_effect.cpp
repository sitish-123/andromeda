#include "server/background_effect.h"

#include "ext-background-effect-v1-protocol.h"
#include "wlr.h"

namespace andromeda {

  namespace {
    constexpr int kVersion = 1;
  }

  const struct ext_background_effect_manager_v1_interface BackgroundEffectManager::kManagerImplementation = {
      .destroy = handleManagerDestroy,
      .get_background_effect = handleGetBackgroundEffect,
  };

  const struct ext_background_effect_surface_v1_interface BackgroundEffectManager::kSurfaceImplementation = {
      .destroy = handleSurfaceDestroy,
      .set_blur_region = handleSetBlurRegion,
  };

  const wlr_addon_interface BackgroundEffectManager::kAddonInterface = {
      .name = "andromeda_background_effect",
      .destroy = handleAddonDestroy,
  };

  BackgroundEffectManager::BackgroundEffectManager(wl_display* display) {
    m_global = wl_global_create(display, &ext_background_effect_manager_v1_interface, kVersion, this, bind);
  }

  BackgroundEffectManager::~BackgroundEffectManager() {
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  const pixman_region32_t* BackgroundEffectManager::blurRegion(wlr_surface* surface) const {
    wlr_addon* addon = wlr_addon_find(&surface->addons, this, &kAddonInterface);
    if (addon == nullptr) {
      return nullptr;
    }
    const Surface* state = wl_container_of(addon, state, addon);
    return pixman_region32_not_empty(&state->current) ? &state->current : nullptr;
  }

  void BackgroundEffectManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &ext_background_effect_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
    ext_background_effect_manager_v1_send_capabilities(resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
  }

  void BackgroundEffectManager::handleManagerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void BackgroundEffectManager::handleGetBackgroundEffect(
      wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource
  ) {
    auto* self = static_cast<BackgroundEffectManager*>(wl_resource_get_user_data(resource));
    wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
    Surface* state = nullptr;
    if (wlr_addon* addon = wlr_addon_find(&surface->addons, self, &kAddonInterface)) {
      state = wl_container_of(addon, state, addon);
      if (state->resource != nullptr) {
        wl_resource_post_error(
            resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
            "the surface already has a background effect object"
        );
        return;
      }
    }

    wl_resource* effect = wl_resource_create(
        client, &ext_background_effect_surface_v1_interface, wl_resource_get_version(resource), id
    );
    if (effect == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    if (state == nullptr) {
      state = new Surface{.manager = self, .surface = surface};
      pixman_region32_init(&state->pending);
      pixman_region32_init(&state->current);
      wlr_addon_init(&state->addon, &surface->addons, self, &kAddonInterface);
      state->clientCommit.notify = handleClientCommit;
      wl_signal_add(&surface->events.client_commit, &state->clientCommit);
    }
    state->resource = effect;
    wl_resource_set_implementation(effect, &kSurfaceImplementation, state, handleSurfaceResourceDestroyed);
  }

  void BackgroundEffectManager::handleSurfaceDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void BackgroundEffectManager::handleSetBlurRegion(wl_client* /*client*/, wl_resource* resource, wl_resource* region) {
    auto* state = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (state == nullptr) {
      wl_resource_post_error(
          resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED, "the surface has been destroyed"
      );
      return;
    }
    if (region != nullptr) {
      pixman_region32_copy(&state->pending, wlr_region_from_resource(region));
    } else {
      pixman_region32_clear(&state->pending);
    }
    state->pendingSet = true;
  }

  void BackgroundEffectManager::handleSurfaceResourceDestroyed(wl_resource* resource) {
    auto* state = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (state == nullptr) {
      return;
    }
    // The region goes away on the next commit, as the protocol says.
    state->resource = nullptr;
    pixman_region32_clear(&state->pending);
    state->pendingSet = true;
  }

  void BackgroundEffectManager::handleClientCommit(wl_listener* listener, void* /*data*/) {
    Surface* state = wl_container_of(listener, state, clientCommit);
    if (!state->pendingSet) {
      return;
    }
    state->pendingSet = false;
    pixman_region32_intersect_rect(
        &state->current, &state->pending, 0, 0, state->surface->pending.width, state->surface->pending.height
    );
    if (state->resource == nullptr && !pixman_region32_not_empty(&state->current)) {
      wlr_addon_finish(&state->addon);
      destroySurface(state);
    }
  }

  void BackgroundEffectManager::handleAddonDestroy(wlr_addon* addon) {
    Surface* state = wl_container_of(addon, state, addon);
    wlr_addon_finish(addon);
    if (state->resource != nullptr) {
      wl_resource_set_user_data(state->resource, nullptr);
    }
    destroySurface(state);
  }

  void BackgroundEffectManager::destroySurface(Surface* state) {
    wl_list_remove(&state->clientCommit.link);
    pixman_region32_fini(&state->pending);
    pixman_region32_fini(&state->current);
    delete state;
  }

} // namespace andromeda
