#include "server/commit_timing.h"

#include "commit-timing-v1-protocol.h"
#include "wlr.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;
    constexpr long kNanosecondsPerSecond = 1'000'000'000;

    std::chrono::nanoseconds sinceEpoch(const timespec& time) {
      return std::chrono::seconds(time.tv_sec) + std::chrono::nanoseconds(time.tv_nsec);
    }

    std::chrono::nanoseconds now() {
      timespec time{};
      clock_gettime(CLOCK_MONOTONIC, &time);
      return sinceEpoch(time);
    }

  } // namespace

  CommitTimingManager::CommitTimingManager(wl_display* display) : m_display(display) {
    m_global = wl_global_create(display, &wp_commit_timing_manager_v1_interface, kVersion, this, bind);
  }

  CommitTimingManager::~CommitTimingManager() {
    while (!m_surfaces.empty()) {
      destroy(m_surfaces.back().get());
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void CommitTimingManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &wp_commit_timing_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
  }

  void CommitTimingManager::handleManagerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void CommitTimingManager::handleGetTimer(
      wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource
  ) {
    auto* self = static_cast<CommitTimingManager*>(wl_resource_get_user_data(resource));
    wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
    Surface* state = nullptr;
    if (wlr_addon* addon = wlr_addon_find(&surface->addons, self, &kAddonInterface)) {
      Surface* existing = wl_container_of(addon, existing, addon);
      if (existing->resource != nullptr) {
        wl_resource_post_error(
            resource, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS, "the surface already has a commit timer"
        );
        return;
      }
      state = existing;
    }
    wl_resource* timer =
        wl_resource_create(client, &wp_commit_timer_v1_interface, wl_resource_get_version(resource), id);
    if (timer == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    if (state == nullptr) {
      auto owned = std::make_unique<Surface>();
      state = owned.get();
      state->manager = self;
      state->surface = surface;
      wlr_addon_init(&state->addon, &surface->addons, self, &kAddonInterface);
      state->clientCommit.notify = handleClientCommit;
      wl_signal_add(&surface->events.client_commit, &state->clientCommit);
      state->timer = wl_event_loop_add_timer(wl_display_get_event_loop(self->m_display), handleTimer, state);
      self->m_surfaces.push_back(std::move(owned));
    }
    state->resource = timer;
    wl_resource_set_implementation(timer, &kTimerImplementation, state, handleTimerResourceDestroyed);
  }

  CommitTimingManager::Surface* CommitTimingManager::surfaceFrom(wl_resource* resource) {
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
  }

  void CommitTimingManager::handleSetTimestamp(
      wl_client* /*client*/, wl_resource* resource, uint32_t secHi, uint32_t secLo, uint32_t nsec
  ) {
    Surface* state = surfaceFrom(resource);
    if (state == nullptr) {
      wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed");
      return;
    }
    if (nsec >= kNanosecondsPerSecond) {
      wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP, "tv_nsec is 1e9 or more");
      return;
    }
    if (state->pending) {
      wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS, "the next commit has a timestamp");
      return;
    }
    const uint64_t seconds = (static_cast<uint64_t>(secHi) << 32U) | secLo;
    state->pending = timespec{
        .tv_sec = static_cast<time_t>(std::min<uint64_t>(seconds, std::numeric_limits<time_t>::max())),
        .tv_nsec = static_cast<long>(nsec),
    };
  }

  void CommitTimingManager::handleTimerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  // Held updates keep their timestamps after the object goes, as the protocol asks; the state goes once none is held.
  void CommitTimingManager::handleTimerResourceDestroyed(wl_resource* resource) {
    Surface* state = surfaceFrom(resource);
    if (state == nullptr) {
      return;
    }
    state->resource = nullptr;
    state->pending.reset();
    if (state->held.empty()) {
      state->manager->destroy(state);
    }
  }

  void CommitTimingManager::handleClientCommit(wl_listener* listener, void* /*data*/) {
    Surface* state = wl_container_of(listener, state, clientCommit);
    if (!state->pending) {
      return;
    }
    const timespec target = *state->pending;
    state->pending.reset();
    if (sinceEpoch(target) <= now()) {
      return;
    }
    state->held.push_back({.target = target, .lock = wlr_surface_lock_pending(state->surface)});
    arm(*state, now());
  }

  // Unlocks every held update whose time has come, then sleeps until the next one's.
  void CommitTimingManager::release(Surface& state) {
    const std::chrono::nanoseconds current = now();
    std::vector<uint32_t> due;
    std::erase_if(state.held, [&](const Held& held) {
      if (sinceEpoch(held.target) > current) {
        return false;
      }
      due.push_back(held.lock);
      return true;
    });
    for (const uint32_t lock : due) {
      wlr_surface_unlock_cached(state.surface, lock);
    }
    arm(state, current);
  }

  void CommitTimingManager::arm(Surface& state, std::chrono::nanoseconds current) {
    if (state.held.empty()) {
      wl_event_source_timer_update(state.timer, 0);
      return;
    }
    const auto next = std::ranges::min(state.held, {}, [](const Held& held) { return sinceEpoch(held.target); });
    // Rounded up, so the timer never fires before the target; 0 would disarm it.
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(sinceEpoch(next.target) - current);
    wl_event_source_timer_update(state.timer, static_cast<int>(std::max<int64_t>(wait.count(), 1)));
  }

  int CommitTimingManager::handleTimer(void* data) {
    auto* state = static_cast<Surface*>(data);
    release(*state);
    if (state->resource == nullptr && state->held.empty()) {
      state->manager->destroy(state);
    }
    return 0;
  }

  void CommitTimingManager::destroy(Surface* state) {
    wl_list_remove(&state->clientCommit.link);
    wlr_addon_finish(&state->addon);
    if (state->timer != nullptr) {
      wl_event_source_remove(state->timer);
    }
    if (state->resource != nullptr) {
      wl_resource_set_user_data(state->resource, nullptr);
    }
    std::erase_if(m_surfaces, [state](const auto& owned) { return owned.get() == state; });
  }

  void CommitTimingManager::handleSurfaceDestroyed(wlr_addon* addon) {
    Surface* state = wl_container_of(addon, state, addon);
    state->manager->destroy(state);
  }

  // The addon also marks a surface that already has timing state.
  const wlr_addon_interface CommitTimingManager::kAddonInterface = {
      .name = "umbriel_commit_timing_v1",
      .destroy = CommitTimingManager::handleSurfaceDestroyed,
  };

  const struct wp_commit_timing_manager_v1_interface CommitTimingManager::kManagerImplementation = {
      .destroy = CommitTimingManager::handleManagerDestroy,
      .get_timer = CommitTimingManager::handleGetTimer,
  };

  const struct wp_commit_timer_v1_interface CommitTimingManager::kTimerImplementation = {
      .set_timestamp = CommitTimingManager::handleSetTimestamp,
      .destroy = CommitTimingManager::handleTimerDestroy,
  };

} // namespace umbriel
