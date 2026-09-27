#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/addon.h>
}

struct wlr_output;
struct wlr_surface;
struct wp_fifo_manager_v1_interface;
struct wp_fifo_v1_interface;

namespace umbriel {

  // Serves wp_fifo_manager_v1: a content update that waits on a surface's barrier is held until an output the surface
  // is on has latched the update that set the barrier. Surfaces on no enabled output ignore the wait, as the protocol
  // allows, so a hidden client throttles on its frame callbacks instead.
  class FifoManager {
  public:
    explicit FifoManager(wl_display* display);
    ~FifoManager();

    FifoManager(const FifoManager&) = delete;
    FifoManager& operator=(const FifoManager&) = delete;

    // After `output` committed a frame: clears the barrier of every surface on it and applies the updates it held.
    void latched(wlr_output* output);

  private:
    struct Update {
      bool setsBarrier = false;
      std::optional<uint32_t> lock;
    };

    // Lives with its surface while its wp_fifo_v1 exists or it still holds a barrier or an update.
    struct Surface {
      FifoManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wlr_addon addon{};
      bool setPending = false;
      bool waitPending = false;
      bool barrier = false;
      bool draining = false;
      // Client commits not yet applied, oldest first.
      std::deque<Update> updates;
      wl_listener clientCommit{};
      wl_listener commit{};
    };

    // The scanner also declares wl_interface variables of these names, so the structs need their elaborated form.
    static const struct wp_fifo_manager_v1_interface kManagerImplementation;
    static const struct wp_fifo_v1_interface kFifoImplementation;
    static const wlr_addon_interface kAddonInterface;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleManagerDestroy(wl_client* client, wl_resource* resource);
    static void handleGetFifo(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource);
    static void handleSetBarrier(wl_client* client, wl_resource* resource);
    static void handleWaitBarrier(wl_client* client, wl_resource* resource);
    static void handleFifoDestroy(wl_client* client, wl_resource* resource);
    static void handleFifoResourceDestroyed(wl_resource* resource);
    static void handleClientCommit(wl_listener* listener, void* data);
    static void handleCommit(wl_listener* listener, void* data);
    static void handleSurfaceDestroyed(wlr_addon* addon);

    static Surface* surfaceFrom(wl_resource* resource);
    static bool onEnabledOutput(const wlr_surface* surface);
    static bool onOutput(const wlr_surface* surface, const wlr_output* output);
    static void drain(Surface& state);
    static bool idle(const Surface& state);
    void destroy(Surface* state);

    wl_global* m_global = nullptr;
    std::vector<std::unique_ptr<Surface>> m_surfaces;
  };

} // namespace umbriel
