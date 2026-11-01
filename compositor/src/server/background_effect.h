#pragma once

#include <pixman.h>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/addon.h>
}

struct wlr_surface;
struct ext_background_effect_manager_v1_interface;
struct ext_background_effect_surface_v1_interface;

namespace andromeda {

  // Serves ext_background_effect_manager_v1: a client names the region of its surface whose background the
  // compositor blurs. The region is double-buffered and applies on the surface's next commit.
  class BackgroundEffectManager {
  public:
    explicit BackgroundEffectManager(wl_display* display);
    ~BackgroundEffectManager();

    BackgroundEffectManager(const BackgroundEffectManager&) = delete;
    BackgroundEffectManager& operator=(const BackgroundEffectManager&) = delete;

    // The committed blur region of `surface` in surface-local coordinates; nullptr when it has none.
    [[nodiscard]] const pixman_region32_t* blurRegion(wlr_surface* surface) const;

  private:
    // Lives with its surface while its protocol object exists or a removal waits for the next commit.
    struct Surface {
      BackgroundEffectManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wlr_addon addon{};
      pixman_region32_t pending{};
      pixman_region32_t current{};
      bool pendingSet = false;
      wl_listener clientCommit{};
    };

    static const struct ext_background_effect_manager_v1_interface kManagerImplementation;
    static const struct ext_background_effect_surface_v1_interface kSurfaceImplementation;
    static const wlr_addon_interface kAddonInterface;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleManagerDestroy(wl_client* client, wl_resource* resource);
    static void handleGetBackgroundEffect(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface);
    static void handleSurfaceDestroy(wl_client* client, wl_resource* resource);
    static void handleSetBlurRegion(wl_client* client, wl_resource* resource, wl_resource* region);
    static void handleSurfaceResourceDestroyed(wl_resource* resource);
    static void handleClientCommit(wl_listener* listener, void* data);
    static void handleAddonDestroy(wlr_addon* addon);
    static void destroySurface(Surface* state);

    wl_global* m_global = nullptr;
  };

} // namespace andromeda
