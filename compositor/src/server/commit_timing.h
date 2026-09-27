#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/addon.h>
}

struct wlr_surface;
struct wp_commit_timing_manager_v1_interface;
struct wp_commit_timer_v1_interface;

namespace umbriel {

  // Serves wp_commit_timing_manager_v1: a content update with a timestamp is held until CLOCK_MONOTONIC, the
  // presentation clock, reaches it, so it is presented at the first refresh at or after that time and never before.
  class CommitTimingManager {
  public:
    explicit CommitTimingManager(wl_display* display);
    ~CommitTimingManager();

    CommitTimingManager(const CommitTimingManager&) = delete;
    CommitTimingManager& operator=(const CommitTimingManager&) = delete;

  private:
    struct Held {
      timespec target{};
      uint32_t lock = 0;
    };

    // Lives with its surface while its wp_commit_timer_v1 exists or it still holds an update.
    struct Surface {
      CommitTimingManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wlr_addon addon{};
      std::optional<timespec> pending;
      std::vector<Held> held;
      wl_event_source* timer = nullptr;
      wl_listener clientCommit{};
    };

    // The scanner also declares wl_interface variables of these names, so the structs need their elaborated form.
    static const struct wp_commit_timing_manager_v1_interface kManagerImplementation;
    static const struct wp_commit_timer_v1_interface kTimerImplementation;
    static const wlr_addon_interface kAddonInterface;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleManagerDestroy(wl_client* client, wl_resource* resource);
    static void handleGetTimer(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource);
    static void
    handleSetTimestamp(wl_client* client, wl_resource* resource, uint32_t secHi, uint32_t secLo, uint32_t nsec);
    static void handleTimerDestroy(wl_client* client, wl_resource* resource);
    static void handleTimerResourceDestroyed(wl_resource* resource);
    static void handleClientCommit(wl_listener* listener, void* data);
    static int handleTimer(void* data);
    static void handleSurfaceDestroyed(wlr_addon* addon);

    static Surface* surfaceFrom(wl_resource* resource);
    static void release(Surface& state);
    static void arm(Surface& state, std::chrono::nanoseconds current);
    void destroy(Surface* state);

    wl_display* m_display = nullptr;
    wl_global* m_global = nullptr;
    std::vector<std::unique_ptr<Surface>> m_surfaces;
  };

} // namespace umbriel
