// dsk_tracker_v1: compositor-owned gestures of DskSurfaceManager (protocol/dsk-surface-v1.xml).
#include "server/dsk_surface.h"

#include "config/config.h"
#include "design_tokens.h"
#include "dsk-surface-v1-protocol.h"
#include "wlr.h"

#include <algorithm>
#include <ctime>

namespace andromeda {

  namespace {
    constexpr uint32_t kAxisCount = 3;
    constexpr uint32_t kPropertyCount = 5;
    constexpr uint32_t kComponentCount = 4;
    constexpr uint32_t kCurveCount = 16;
  } // namespace

  const struct dsk_tracker_v1_interface DskSurfaceManager::kTrackerImplementation = {
      .destroy = handleTrackerDestroy,
      .set_source = handleSetSource,
      .set_range = handleSetRange,
      .add_snap = handleAddSnap,
      .bind = handleBind,
      .move_to = handleMoveTo,
      .watch = handleWatch,
  };

  DskSurfaceManager::Tracker* DskSurfaceManager::trackerFromResource(wl_resource* resource) {
    return static_cast<Tracker*>(wl_resource_get_user_data(resource));
  }

  void DskSurfaceManager::handleTrackerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void DskSurfaceManager::handleSetSource(
      wl_client* /*client*/, wl_resource* resource, uint32_t axis, uint32_t fingers, wl_resource* region
  ) {
    Tracker* tracker = trackerFromResource(resource);
    if (tracker == nullptr) {
      return;
    }
    if (axis >= kAxisCount) {
      wl_resource_post_error(resource, DSK_TRACKER_V1_ERROR_INVALID_VALUE, "invalid axis %u", axis);
      return;
    }
    tracker->pending.axis = axis;
    tracker->pending.fingers = fingers;
    tracker->pending.region.clear();
    if (region != nullptr) {
      int count = 0;
      const pixman_box32_t* boxes = pixman_region32_rectangles(wlr_region_from_resource(region), &count);
      tracker->pending.region.assign(boxes, boxes + count);
    }
    tracker->pendingChanged = true;
  }

  void DskSurfaceManager::handleSetRange(wl_client* /*client*/, wl_resource* resource, int32_t minimum, int32_t maximum) {
    if (Tracker* tracker = trackerFromResource(resource)) {
      tracker->pending.minimum = wl_fixed_to_double(minimum);
      tracker->pending.maximum = wl_fixed_to_double(maximum);
      tracker->pendingChanged = true;
    }
  }

  void DskSurfaceManager::handleAddSnap(wl_client* /*client*/, wl_resource* resource, int32_t position) {
    if (Tracker* tracker = trackerFromResource(resource)) {
      tracker->pending.snaps.push_back(wl_fixed_to_double(position));
      tracker->pendingChanged = true;
    }
  }

  void DskSurfaceManager::handleBind(
      wl_client* /*client*/, wl_resource* resource, wl_resource* target, uint32_t property, uint32_t component, int32_t p0,
      int32_t p1, int32_t v0, int32_t v1
  ) {
    Tracker* tracker = trackerFromResource(resource);
    auto* targetState = static_cast<Surface*>(wl_resource_get_user_data(target));
    if (tracker == nullptr || targetState == nullptr) {
      return;
    }
    if (property >= kPropertyCount || component >= kComponentCount) {
      wl_resource_post_error(
          resource, DSK_TRACKER_V1_ERROR_INVALID_VALUE, "invalid property %u or component %u", property, component
      );
      return;
    }
    tracker->pending.bindings.push_back(
        {.target = targetState,
         .property = static_cast<SurfaceMotion::Property>(property),
         .component = component,
         .p0 = wl_fixed_to_double(p0),
         .p1 = wl_fixed_to_double(p1),
         .v0 = wl_fixed_to_double(v0),
         .v1 = wl_fixed_to_double(v1)}
    );
    tracker->pendingChanged = true;
  }

  void DskSurfaceManager::handleMoveTo(wl_client* /*client*/, wl_resource* resource, int32_t position, uint32_t curve) {
    Tracker* tracker = trackerFromResource(resource);
    if (tracker == nullptr) {
      return;
    }
    if (curve >= kCurveCount) {
      wl_resource_post_error(resource, DSK_TRACKER_V1_ERROR_INVALID_VALUE, "invalid curve %u", curve);
      return;
    }
    tracker->pendingMove = std::pair{wl_fixed_to_double(position), static_cast<SurfaceMotion::Curve>(curve)};
  }

  void DskSurfaceManager::handleWatch(wl_client* /*client*/, wl_resource* resource, uint32_t enabled) {
    if (Tracker* tracker = trackerFromResource(resource)) {
      tracker->pending.watch = enabled != 0;
      tracker->pendingChanged = true;
    }
  }

  void DskSurfaceManager::handleTrackerCommit(wl_listener* listener, void* /*data*/) {
    Tracker* tracker = wl_container_of(listener, tracker, clientCommit);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const auto ms = static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
    if (tracker->pendingChanged) {
      // Bindings and a new source arrive during a gesture (the client binds on interacting); only a new range or
      // new snaps restart the motion.
      const bool restart = tracker->pending.minimum != tracker->current.minimum
          || tracker->pending.maximum != tracker->current.maximum || tracker->pending.snaps != tracker->current.snaps;
      tracker->current = tracker->pending;
      tracker->pendingChanged = false;
      if (restart) {
        tracker->motion = TrackerMotion{};
        tracker->motion.setRange(tracker->current.minimum, tracker->current.maximum);
        for (double snap : tracker->current.snaps) {
          tracker->motion.addSnap(snap);
        }
      } else if (tracker->motion.phase() != TrackerMotion::Phase::Idle) {
        tracker->manager->applyTracker(*tracker, ms);
      }
    }
    if (tracker->pendingMove) {
      const auto [target, curve] = *tracker->pendingMove;
      if (curve == SurfaceMotion::Curve::None || config().appearance.reduceMotion) {
        tracker->motion.jumpTo(target, ms);
      } else {
        tracker->motion.moveTo(target, SurfaceMotion::springConfig(curve), ms);
      }
      tracker->pendingMove.reset();
      tracker->manager->applyTracker(*tracker, ms);
    }
  }

  void DskSurfaceManager::applyTracker(Tracker& tracker, uint32_t timeMs) const {
    const double position = tracker.motion.position();
    for (const Binding& binding : tracker.current.bindings) {
      const double span = binding.p1 - binding.p0;
      const double t = span != 0.0 ? std::clamp((position - binding.p0) / span, 0.0, 1.0) : 1.0;
      SurfaceMotion::Value value = binding.target->current.motion.value(binding.property);
      value[binding.component] = binding.v0 + (binding.v1 - binding.v0) * t;
      binding.target->current.motion.set(binding.property, value);
      sendEvents(*binding.target, timeMs);
    }
    for (const TrackerMotion::Event& event : tracker.motion.takeEvents()) {
      switch (event.kind) {
      case TrackerMotion::Event::Kind::Interacting:
        dsk_tracker_v1_send_interacting(tracker.resource, event.timeMs);
        break;
      case TrackerMotion::Event::Kind::Inertia:
        dsk_tracker_v1_send_inertia(tracker.resource, event.timeMs, wl_fixed_from_double(event.position));
        break;
      case TrackerMotion::Event::Kind::Idle:
        dsk_tracker_v1_send_idle(tracker.resource, wl_fixed_from_double(event.position));
        break;
      }
    }
    if (tracker.current.watch && tracker.motion.phase() != TrackerMotion::Phase::Idle) {
      dsk_tracker_v1_send_position(tracker.resource, timeMs, wl_fixed_from_double(position));
    }
  }

  DskSurfaceManager::Tracker*
  DskSurfaceManager::findTracker(wlr_surface* surface, double sx, double sy, uint32_t fingers) const {
    const auto inside = [sx, sy](const Tracker& tracker) {
      return tracker.current.region.empty()
          || std::ranges::any_of(tracker.current.region, [sx, sy](const pixman_box32_t& box) {
               return sx >= box.x1 && sx < box.x2 && sy >= box.y1 && sy < box.y2;
             });
    };
    const auto found = std::ranges::find_if(m_trackers, [&](const Tracker* tracker) {
      return tracker->surface == surface
          && tracker->current.axis != DSK_TRACKER_V1_AXIS_PINCH
          && tracker->current.fingers == fingers
          && inside(*tracker);
    });
    return found != m_trackers.end() ? *found : nullptr;
  }

  std::optional<bool> DskSurfaceManager::dragAxisHorizontal(wlr_surface* surface, double sx, double sy) const {
    const Tracker* tracker = findTracker(surface, sx, sy, 0);
    return tracker != nullptr ? std::optional{tracker->current.axis == DSK_TRACKER_V1_AXIS_HORIZONTAL} : std::nullopt;
  }

  bool DskSurfaceManager::beginSwipe(wlr_surface* surface, double sx, double sy, uint32_t fingers, uint32_t timeMs) {
    m_swipe = findTracker(surface, sx, sy, fingers);
    if (m_swipe == nullptr) {
      return false;
    }
    m_swipe->motion.begin(timeMs);
    applyTracker(*m_swipe, timeMs);
    return true;
  }

  bool DskSurfaceManager::beginPinch(wlr_surface* surface, double sx, double sy, uint32_t fingers, uint32_t timeMs) {
    const auto found = std::ranges::find_if(m_trackers, [&](const Tracker* tracker) {
      return tracker->surface == surface
          && tracker->current.axis == DSK_TRACKER_V1_AXIS_PINCH
          && tracker->current.fingers == fingers
          && (tracker->current.region.empty()
              || std::ranges::any_of(tracker->current.region, [sx, sy](const pixman_box32_t& box) {
                   return sx >= box.x1 && sx < box.x2 && sy >= box.y1 && sy < box.y2;
                 }));
    });
    m_swipe = found != m_trackers.end() ? *found : nullptr;
    if (m_swipe == nullptr) {
      return false;
    }
    m_pinchScale = 1.0;
    m_swipe->motion.begin(timeMs);
    applyTracker(*m_swipe, timeMs);
    return true;
  }

  void DskSurfaceManager::updatePinch(double scale, uint32_t timeMs) {
    if (m_swipe == nullptr) {
      return;
    }
    // A pinch from scale 1 to 2 covers the whole range; pinching in moves back.
    const double span = m_swipe->current.maximum - m_swipe->current.minimum;
    m_swipe->motion.update((scale - m_pinchScale) * span, timeMs);
    m_pinchScale = scale;
    applyTracker(*m_swipe, timeMs);
  }

  void DskSurfaceManager::updateSwipe(double dx, double dy, uint32_t timeMs) {
    if (m_swipe == nullptr) {
      return;
    }
    m_swipe->motion.update(m_swipe->current.axis == DSK_TRACKER_V1_AXIS_HORIZONTAL ? dx : dy, timeMs);
    applyTracker(*m_swipe, timeMs);
  }

  void DskSurfaceManager::endSwipe(uint32_t timeMs) {
    if (m_swipe == nullptr) {
      return;
    }
    m_swipe->motion.end(timeMs);
    applyTracker(*m_swipe, timeMs);
    m_swipe = nullptr;
  }

  void DskSurfaceManager::cancelSwipe() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    endSwipe(static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000));
  }

  void DskSurfaceManager::handleTrackerSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    Tracker* tracker = wl_container_of(listener, tracker, surfaceDestroy);
    wl_list_remove(&tracker->clientCommit.link);
    wl_list_remove(&tracker->surfaceDestroy.link);
    tracker->surface = nullptr;
    tracker->current = {};
    if (tracker->manager->m_swipe == tracker) {
      tracker->manager->m_swipe = nullptr;
    }
  }

  void DskSurfaceManager::handleTrackerResourceDestroyed(wl_resource* resource) {
    Tracker* tracker = trackerFromResource(resource);
    if (tracker->surface != nullptr) {
      wl_list_remove(&tracker->clientCommit.link);
      wl_list_remove(&tracker->surfaceDestroy.link);
    }
    DskSurfaceManager* manager = tracker->manager;
    std::erase(manager->m_trackers, tracker);
    if (manager->m_swipe == tracker) {
      manager->m_swipe = nullptr;
    }
    delete tracker;
  }

} // namespace andromeda
