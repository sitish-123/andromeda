#include "server/dsk_surface.h"

#include "config/config.h"
#include "dsk-surface-v1-protocol.h"
#include "wlr.h"

#include <algorithm>
#include <ctime>

namespace andromeda {

  namespace {
    constexpr int kVersion = 9;
    constexpr uint32_t kPropertyCount = 9;
    constexpr uint32_t kCurveCount = 16;
    constexpr uint32_t kElevationCount = 3;

    constexpr int kElevationPropertySince = 4;
    constexpr int kLiquidPropertySince = 5;
    constexpr int kStrokePropertySince = 7;
    constexpr int kTintPropertySince = 9;

    bool validProperty(wl_resource* resource, uint32_t property) {
      const int version = wl_resource_get_version(resource);
      return property < kPropertyCount
          && (property != static_cast<uint32_t>(SurfaceMotion::Property::Elevation) || version >= kElevationPropertySince)
          && (property != static_cast<uint32_t>(SurfaceMotion::Property::Liquid) || version >= kLiquidPropertySince)
          && (property != static_cast<uint32_t>(SurfaceMotion::Property::Stroke) || version >= kStrokePropertySince)
          && (property != static_cast<uint32_t>(SurfaceMotion::Property::Tint) || version >= kTintPropertySince);
    }

    SurfaceMotion::Value toValue(int32_t a, int32_t b, int32_t c, int32_t d) {
      return {wl_fixed_to_double(a), wl_fixed_to_double(b), wl_fixed_to_double(c), wl_fixed_to_double(d)};
    }
  } // namespace

  const struct dsk_surface_manager_v1_interface DskSurfaceManager::kManagerImplementation = {
      .destroy = handleManagerDestroy,
      .get_surface = handleGetSurface,
      .get_tracker = handleGetTracker,
  };

  const struct dsk_surface_v1_interface DskSurfaceManager::kSurfaceImplementation = {
      .destroy = handleSurfaceDestroy,
      .set_elevation = handleSetElevation,
      .set = handleSet,
      .animate = handleAnimate,
      .set_clip_from = handleSetClipFrom,
      .set_visible_region = handleSetVisibleRegion,
  };

  const wlr_addon_interface DskSurfaceManager::kAddonInterface = {
      .name = "andromeda_dsk_surface",
      .destroy = handleAddonDestroy,
  };

  DskSurfaceManager::DskSurfaceManager(wl_display* display) {
    m_global = wl_global_create(display, &dsk_surface_manager_v1_interface, kVersion, this, bind);
  }

  DskSurfaceManager::~DskSurfaceManager() {
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DskSurfaceManager::sendBackdrop(wlr_surface* surface, bool dark) const {
    wlr_addon* addon = wlr_addon_find(&surface->addons, this, &kAddonInterface);
    if (addon == nullptr) {
      return;
    }
    Surface* state = wl_container_of(addon, state, addon);
    const int tone = dark ? DSK_SURFACE_V1_BACKDROP_DARK : DSK_SURFACE_V1_BACKDROP_LIGHT;
    if (state->current.backdropTone == tone
        || wl_resource_get_version(state->resource) < DSK_SURFACE_V1_BACKDROP_SINCE_VERSION) {
      return;
    }
    state->current.backdropTone = tone;
    dsk_surface_v1_send_backdrop(state->resource, static_cast<uint32_t>(tone));
  }

  DskSurfaceManager::State* DskSurfaceManager::state(wlr_surface* surface) const {
    wlr_addon* addon = wlr_addon_find(&surface->addons, this, &kAddonInterface);
    if (addon == nullptr) {
      return nullptr;
    }
    Surface* state = wl_container_of(addon, state, addon);
    return &state->current;
  }

  bool DskSurfaceManager::tracks(const Tracker& tracker, wlr_surface* surface) {
    return tracker.surface == surface
        || std::ranges::any_of(tracker.current.bindings, [surface](const Binding& binding) {
             return binding.target->surface == surface;
           });
  }

  bool DskSurfaceManager::tick(wlr_surface* surface, uint32_t nowMs) const {
    bool changed = false;
    // A tracker ticks from its own surface and from each bound one; the second tick in a frame has dt 0.
    for (Tracker* tracker : m_trackers) {
      if (tracks(*tracker, surface) && tracker->motion.tick(nowMs)) {
        applyTracker(*tracker, nowMs);
        changed = true;
      }
    }
    wlr_addon* addon = wlr_addon_find(&surface->addons, this, &kAddonInterface);
    if (addon == nullptr) {
      return changed;
    }
    Surface* state = wl_container_of(addon, state, addon);
    changed = state->current.motion.tick(nowMs) || changed;
    sendEvents(*state, nowMs);
    return changed;
  }

  bool DskSurfaceManager::driven(wlr_surface* surface) const {
    return std::ranges::any_of(m_trackers, [surface](const Tracker* tracker) {
      return tracker->motion.phase() != TrackerMotion::Phase::Idle && tracks(*tracker, surface);
    });
  }

  void DskSurfaceManager::sendEvents(Surface& state, uint32_t timeMs) {
    for (const SurfaceMotion::Event& event : state.current.motion.takeEvents()) {
      if (event.kind == SurfaceMotion::Event::Kind::Started) {
        dsk_surface_v1_send_started(state.resource, event.serial, timeMs);
      } else {
        dsk_surface_v1_send_done(state.resource, event.serial, static_cast<uint32_t>(event.reason));
      }
    }
  }

  DskSurfaceManager::Surface* DskSurfaceManager::fromResource(wl_resource* resource) {
    auto* state = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (state == nullptr) {
      wl_resource_post_error(resource, DSK_SURFACE_V1_ERROR_DEFUNCT, "the surface has been destroyed");
    }
    return state;
  }

  void DskSurfaceManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &dsk_surface_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
  }

  void DskSurfaceManager::handleManagerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void DskSurfaceManager::handleGetSurface(
      wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource
  ) {
    auto* self = static_cast<DskSurfaceManager*>(wl_resource_get_user_data(resource));
    wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
    if (wlr_addon_find(&surface->addons, self, &kAddonInterface) != nullptr) {
      wl_resource_post_error(
          resource, DSK_SURFACE_MANAGER_V1_ERROR_ALREADY_EXTENDED, "the surface already has a dsk_surface_v1"
      );
      return;
    }
    wl_resource* object = wl_resource_create(client, &dsk_surface_v1_interface, wl_resource_get_version(resource), id);
    if (object == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    auto* state = new Surface{.manager = self, .resource = object, .surface = surface};
    state->current.version = wl_resource_get_version(object);
    wlr_addon_init(&state->addon, &surface->addons, self, &kAddonInterface);
    state->clientCommit.notify = handleClientCommit;
    wl_signal_add(&surface->events.client_commit, &state->clientCommit);
    wl_resource_set_implementation(object, &kSurfaceImplementation, state, handleSurfaceResourceDestroyed);
  }

  void DskSurfaceManager::handleGetTracker(
      wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surfaceResource
  ) {
    auto* self = static_cast<DskSurfaceManager*>(wl_resource_get_user_data(resource));
    wl_resource* object = wl_resource_create(client, &dsk_tracker_v1_interface, wl_resource_get_version(resource), id);
    if (object == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    auto* tracker = new Tracker{.manager = self, .resource = object, .surface = wlr_surface_from_resource(surfaceResource)};
    tracker->clientCommit.notify = handleTrackerCommit;
    wl_signal_add(&tracker->surface->events.client_commit, &tracker->clientCommit);
    tracker->surfaceDestroy.notify = handleTrackerSurfaceDestroy;
    wl_signal_add(&tracker->surface->events.destroy, &tracker->surfaceDestroy);
    self->m_trackers.push_back(tracker);
    wl_resource_set_implementation(object, &kTrackerImplementation, tracker, handleTrackerResourceDestroyed);
  }

  void DskSurfaceManager::handleSurfaceDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void DskSurfaceManager::handleSetElevation(wl_client* /*client*/, wl_resource* resource, uint32_t level) {
    Surface* state = fromResource(resource);
    if (state == nullptr) {
      return;
    }
    if (level >= kElevationCount) {
      wl_resource_post_error(resource, DSK_SURFACE_V1_ERROR_INVALID_VALUE, "invalid elevation %u", level);
      return;
    }
    state->pending.push_back(
        {.kind = Op::Kind::Set,
         .serial = 0,
         .property = SurfaceMotion::Property::Elevation,
         .curve = SurfaceMotion::Curve::None,
         .value = {static_cast<double>(level), 0.0, 0.0, 0.0}}
    );
  }

  void DskSurfaceManager::handleSet(
      wl_client* /*client*/, wl_resource* resource, uint32_t property, int32_t a, int32_t b, int32_t c, int32_t d
  ) {
    Surface* state = fromResource(resource);
    if (state == nullptr) {
      return;
    }
    if (!validProperty(resource, property)) {
      wl_resource_post_error(resource, DSK_SURFACE_V1_ERROR_INVALID_VALUE, "invalid property %u", property);
      return;
    }
    state->pending.push_back(
        {.kind = Op::Kind::Set,
         .serial = 0,
         .property = static_cast<SurfaceMotion::Property>(property),
         .curve = SurfaceMotion::Curve::None,
         .value = toValue(a, b, c, d)}
    );
  }

  void DskSurfaceManager::handleSetClipFrom(
      wl_client* /*client*/, wl_resource* resource, wl_resource* source, int32_t x, int32_t y, int32_t width,
      int32_t height
  ) {
    Surface* state = fromResource(resource);
    if (state == nullptr || !state->manager->m_originLookup) {
      return;
    }
    // The source is mapped already; this surface may map with this very commit, so its origin is read then.
    wlr_surface* sourceSurface = wlr_surface_from_resource(source);
    const auto from = sourceSurface->mapped ? state->manager->m_originLookup(sourceSurface) : std::nullopt;
    if (!from) {
      return;
    }
    state->pending.push_back(
        {.kind = Op::Kind::ClipFrom,
         .serial = 0,
         .property = SurfaceMotion::Property::Clip,
         .curve = SurfaceMotion::Curve::None,
         .value = {wl_fixed_to_double(x) + from->first, wl_fixed_to_double(y) + from->second,
                   wl_fixed_to_double(width), wl_fixed_to_double(height)}}
    );
  }

  void DskSurfaceManager::handleAnimate(
      wl_client* /*client*/, wl_resource* resource, uint32_t serial, uint32_t property, uint32_t curve, int32_t a,
      int32_t b, int32_t c, int32_t d
  ) {
    Surface* state = fromResource(resource);
    if (state == nullptr) {
      return;
    }
    if (!validProperty(resource, property) || curve >= kCurveCount) {
      wl_resource_post_error(
          resource, DSK_SURFACE_V1_ERROR_INVALID_VALUE, "invalid property %u or curve %u", property, curve
      );
      return;
    }
    state->pending.push_back(
        {.kind = Op::Kind::Animate,
         .serial = serial,
         .property = static_cast<SurfaceMotion::Property>(property),
         .curve = static_cast<SurfaceMotion::Curve>(curve),
         .value = toValue(a, b, c, d)}
    );
  }

  void DskSurfaceManager::handleSetVisibleRegion(wl_client* /*client*/, wl_resource* resource, wl_resource* region) {
    Surface* state = fromResource(resource);
    if (state == nullptr) {
      return;
    }
    if (region == nullptr) {
      state->pendingVisibleRegion.emplace();
      return;
    }
    int count = 0;
    const pixman_box32_t* boxes = pixman_region32_rectangles(wlr_region_from_resource(region), &count);
    state->pendingVisibleRegion = std::vector<pixman_box32_t>(boxes, boxes + count);
  }

  void DskSurfaceManager::handleSurfaceResourceDestroyed(wl_resource* resource) {
    auto* state = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (state == nullptr) {
      return;
    }
    // Animations stop at their current value: the state goes with the protocol object.
    wlr_addon_finish(&state->addon);
    destroySurfaceState(state);
  }

  void DskSurfaceManager::destroySurfaceState(Surface* state) {
    for (Tracker* tracker : state->manager->m_trackers) {
      for (TrackerConfig* config : {&tracker->pending, &tracker->current}) {
        std::erase_if(config->bindings, [state](const Binding& binding) { return binding.target == state; });
      }
    }
    wl_list_remove(&state->clientCommit.link);
    delete state;
  }

  void DskSurfaceManager::handleClientCommit(wl_listener* listener, void* /*data*/) {
    Surface* state = wl_container_of(listener, state, clientCommit);
    for (const Op& op : state->pending) {
      if (op.kind == Op::Kind::ClipFrom) {
        const auto to = state->manager->m_originLookup ? state->manager->m_originLookup(state->surface) : std::nullopt;
        if (!to) {
          continue;
        }
        const SurfaceMotion::Value clip{op.value[0] - to->first, op.value[1] - to->second, op.value[2], op.value[3]};
        dsk_surface_v1_send_clip_mapped(
            state->resource, wl_fixed_from_double(clip[0]), wl_fixed_from_double(clip[1]), wl_fixed_from_double(clip[2]),
            wl_fixed_from_double(clip[3])
        );
        state->current.motion.set(op.property, clip);
      } else if (op.kind == Op::Kind::Set) {
        state->current.motion.set(op.property, op.value);
      } else {
        // Reduce motion: spatial properties jump; opacity keeps its fade.
        const bool still = config().appearance.reduceMotion && op.property != SurfaceMotion::Property::Opacity;
        state->current.motion.animate(op.serial, op.property, still ? SurfaceMotion::Curve::None : op.curve, op.value);
      }
    }
    state->pending.clear();
    if (state->pendingVisibleRegion) {
      state->current.visibleRegion = std::move(*state->pendingVisibleRegion);
      state->pendingVisibleRegion.reset();
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    sendEvents(*state, static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000));
  }

  void DskSurfaceManager::handleAddonDestroy(wlr_addon* addon) {
    Surface* state = wl_container_of(addon, state, addon);
    wlr_addon_finish(addon);
    wl_resource_set_user_data(state->resource, nullptr);
    destroySurfaceState(state);
  }

} // namespace andromeda
