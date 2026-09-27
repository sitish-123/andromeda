#include "server/fifo.h"

#include "fifo-v1-protocol.h"
#include "wlr.h"

#include <algorithm>
#include <ranges>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

  } // namespace

  FifoManager::FifoManager(wl_display* display) {
    m_global = wl_global_create(display, &wp_fifo_manager_v1_interface, kVersion, this, bind);
  }

  FifoManager::~FifoManager() {
    while (!m_surfaces.empty()) {
      destroy(m_surfaces.back().get());
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void FifoManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wp_fifo_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
  }

  void FifoManager::handleManagerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void FifoManager::handleGetFifo(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource) {
    auto* self = static_cast<FifoManager*>(wl_resource_get_user_data(resource));
    wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
    Surface* state = nullptr;
    if (wlr_addon* addon = wlr_addon_find(&surface->addons, self, &kAddonInterface)) {
      Surface* existing = wl_container_of(addon, existing, addon);
      if (existing->resource != nullptr) {
        wl_resource_post_error(resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS, "the surface already has a fifo");
        return;
      }
      state = existing;
    }
    wl_resource* fifo = wl_resource_create(client, &wp_fifo_v1_interface, wl_resource_get_version(resource), id);
    if (fifo == nullptr) {
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
      state->commit.notify = handleCommit;
      wl_signal_add(&surface->events.commit, &state->commit);
      self->m_surfaces.push_back(std::move(owned));
    }
    state->resource = fifo;
    wl_resource_set_implementation(fifo, &kFifoImplementation, state, handleFifoResourceDestroyed);
  }

  FifoManager::Surface* FifoManager::surfaceFrom(wl_resource* resource) {
    return static_cast<Surface*>(wl_resource_get_user_data(resource));
  }

  void FifoManager::handleSetBarrier(wl_client* /*client*/, wl_resource* resource) {
    Surface* state = surfaceFrom(resource);
    if (state == nullptr) {
      wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed");
      return;
    }
    state->setPending = true;
  }

  void FifoManager::handleWaitBarrier(wl_client* /*client*/, wl_resource* resource) {
    Surface* state = surfaceFrom(resource);
    if (state == nullptr) {
      wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface was destroyed");
      return;
    }
    state->waitPending = true;
  }

  void FifoManager::handleFifoDestroy(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

  // Barriers and held updates outlive the object, as the protocol asks; the state goes once it is idle.
  void FifoManager::handleFifoResourceDestroyed(wl_resource* resource) {
    Surface* state = surfaceFrom(resource);
    if (state == nullptr) {
      return;
    }
    state->resource = nullptr;
    state->setPending = false;
    state->waitPending = false;
    if (idle(*state)) {
      state->manager->destroy(state);
    }
  }

  bool FifoManager::onEnabledOutput(const wlr_surface* surface) {
    const wlr_surface_output* entry;
    wl_list_for_each(entry, &surface->current_outputs, link) {
      if (entry->output->enabled) {
        return true;
      }
    }
    return false;
  }

  bool FifoManager::onOutput(const wlr_surface* surface, const wlr_output* output) {
    const wlr_surface_output* entry;
    wl_list_for_each(entry, &surface->current_outputs, link) {
      if (entry->output == output) {
        return true;
      }
    }
    return false;
  }

  void FifoManager::handleClientCommit(wl_listener* listener, void* /*data*/) {
    Surface* state = wl_container_of(listener, state, clientCommit);
    Update update{.setsBarrier = state->setPending, .lock = std::nullopt};
    const bool wait = state->waitPending;
    state->setPending = false;
    state->waitPending = false;
    const bool barrierAhead = state->barrier || std::ranges::any_of(state->updates, &Update::setsBarrier);
    const wlr_subsurface* subsurface = wlr_subsurface_try_from_wlr_surface(state->surface);
    const bool synchronized = subsurface != nullptr && subsurface->synchronized;
    if (wait && barrierAhead && !synchronized && onEnabledOutput(state->surface)) {
      update.lock = wlr_surface_lock_pending(state->surface);
      // A barrier clears only at a latch, and nothing else may be about to draw this output.
      wlr_surface_output* entry;
      wl_list_for_each(entry, &state->surface->current_outputs, link) { wlr_output_schedule_frame(entry->output); }
    }
    state->updates.push_back(update);
  }

  void FifoManager::handleCommit(wl_listener* listener, void* /*data*/) {
    Surface* state = wl_container_of(listener, state, commit);
    if (state->updates.empty()) {
      return;
    }
    if (state->updates.front().setsBarrier) {
      state->barrier = true;
    }
    state->updates.pop_front();
    drain(*state);
  }

  // Applies held updates from the front while no barrier stands; applying one may set the next barrier.
  void FifoManager::drain(Surface& state) {
    if (state.draining) {
      return;
    }
    state.draining = true;
    while (!state.barrier && !state.updates.empty() && state.updates.front().lock) {
      const uint32_t seq = *state.updates.front().lock;
      state.updates.front().lock.reset();
      wlr_surface_unlock_cached(state.surface, seq);
    }
    state.draining = false;
  }

  bool FifoManager::idle(const Surface& state) {
    return state.resource == nullptr
        && !state.barrier
        && std::ranges::none_of(state.updates, [](const Update& update) { return update.lock.has_value(); });
  }

  void FifoManager::latched(wlr_output* output) {
    std::vector<Surface*> finished;
    for (const auto& state : m_surfaces) {
      if (!state->barrier) {
        continue;
      }
      // Off-screen surfaces may ignore their barrier, and one that left every output must not stay stuck.
      if (onOutput(state->surface, output) || !onEnabledOutput(state->surface)) {
        state->barrier = false;
        drain(*state);
      }
      if (idle(*state)) {
        finished.push_back(state.get());
      }
    }
    for (Surface* state : finished) {
      destroy(state);
    }
  }

  void FifoManager::destroy(Surface* state) {
    wl_list_remove(&state->clientCommit.link);
    wl_list_remove(&state->commit.link);
    wlr_addon_finish(&state->addon);
    if (state->resource != nullptr) {
      wl_resource_set_user_data(state->resource, nullptr);
    }
    std::erase_if(m_surfaces, [state](const auto& owned) { return owned.get() == state; });
  }

  void FifoManager::handleSurfaceDestroyed(wlr_addon* addon) {
    Surface* state = wl_container_of(addon, state, addon);
    state->manager->destroy(state);
  }

  // The addon also marks a surface that already has fifo state.
  const wlr_addon_interface FifoManager::kAddonInterface = {
      .name = "umbriel_fifo_v1",
      .destroy = FifoManager::handleSurfaceDestroyed,
  };

  const struct wp_fifo_manager_v1_interface FifoManager::kManagerImplementation = {
      .destroy = FifoManager::handleManagerDestroy,
      .get_fifo = FifoManager::handleGetFifo,
  };

  const struct wp_fifo_v1_interface FifoManager::kFifoImplementation = {
      .set_barrier = FifoManager::handleSetBarrier,
      .wait_barrier = FifoManager::handleWaitBarrier,
      .destroy = FifoManager::handleFifoDestroy,
  };

} // namespace umbriel
