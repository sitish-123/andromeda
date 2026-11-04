#include "wayland/dsk_surface_client.h"

#include "dsk-surface-v1-client-protocol.h"
#include "render/animation/motion_service.h"

#include <utility>

namespace {
  wl_fixed_t fixed(double value) { return wl_fixed_from_double(value); }
} // namespace

const dsk_surface_v1_listener DskSurfaceClient::kListener = {
    .started = handleStarted,
    .done = handleDone,
    .clip_mapped = handleClipMapped,
    .backdrop = handleBackdrop,
};

DskSurfaceClient::DskSurfaceClient(dsk_surface_manager_v1* manager, wl_surface* surface)
    : m_surface(dsk_surface_manager_v1_get_surface(manager, surface)) {
  dsk_surface_v1_add_listener(m_surface, &kListener, this);
}

DskSurfaceClient::~DskSurfaceClient() { dsk_surface_v1_destroy(m_surface); }

void DskSurfaceClient::setElevation(Elevation level) {
  dsk_surface_v1_set_elevation(m_surface, static_cast<uint32_t>(level));
}

void DskSurfaceClient::set(Property property, const Value& value) {
  dsk_surface_v1_set(
      m_surface, static_cast<uint32_t>(property), fixed(value[0]), fixed(value[1]), fixed(value[2]), fixed(value[3])
  );
}

bool DskSurfaceClient::setVisibleRegion(wl_region* region) {
  if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(m_surface)) < DSK_SURFACE_V1_SET_VISIBLE_REGION_SINCE_VERSION) {
    return false;
  }
  dsk_surface_v1_set_visible_region(m_surface, region);
  return true;
}

bool DskSurfaceClient::setClipFrom(wl_surface* source, const Value& rect) {
  if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(m_surface)) < DSK_SURFACE_V1_SET_CLIP_FROM_SINCE_VERSION) {
    return false;
  }
  dsk_surface_v1_set_clip_from(m_surface, source, fixed(rect[0]), fixed(rect[1]), fixed(rect[2]), fixed(rect[3]));
  return true;
}

DskSurfaceClient::Curve DskSurfaceClient::compatCurve(Curve curve, uint32_t version) {
  if (version >= 3 || static_cast<uint32_t>(curve) < static_cast<uint32_t>(Curve::SpringOpen)) {
    return curve;
  }
  switch (curve) {
  case Curve::SpringClose:
  case Curve::SpringPress:
  case Curve::SpringEffectsFast:
    return Curve::SpringSnappy;
  case Curve::SpringOpen:
  case Curve::SpringIslandExpand:
  case Curve::SpringRelease:
    return Curve::SpringBouncy;
  default:
    return Curve::SpringStandard;
  }
}

bool DskSurfaceClient::hasElevationProperty() const {
  return dsk_surface_v1_get_version(m_surface) >= 4;
}

bool DskSurfaceClient::hasLiquidProperty() const { return dsk_surface_v1_get_version(m_surface) >= 5; }

bool DskSurfaceClient::hasStrokeProperty() const { return dsk_surface_v1_get_version(m_surface) >= 7; }

bool DskSurfaceClient::hasTintProperty() const { return dsk_surface_v1_get_version(m_surface) >= 9; }

bool DskSurfaceClient::managerHasTint(dsk_surface_manager_v1* manager) {
  return manager != nullptr && dsk_surface_manager_v1_get_version(manager) >= 9;
}

DskSurfaceClient::Curve DskSurfaceClient::effectiveCurve(Curve curve) const {
  return compatCurve(curve, dsk_surface_v1_get_version(m_surface));
}

void DskSurfaceClient::animate(
    Property property, Curve curve, const Value& to, DoneCallback onDone, StartedCallback onStarted
) {
  // Rulebook 7.8: animations off make every change instant; reduced motion keeps only the opacity crossfade.
  const auto& motion = MotionService::instance();
  if (!motion.enabled() || (motion.reduced() && property != Property::Opacity)) {
    curve = Curve::None;
  }
  const uint32_t serial = m_nextSerial++;
  if (onDone) {
    m_pending.emplace(serial, std::move(onDone));
  }
  if (onStarted) {
    m_started.emplace(serial, std::move(onStarted));
  }
  dsk_surface_v1_animate(
      m_surface, serial, static_cast<uint32_t>(property),
      static_cast<uint32_t>(effectiveCurve(curve)), fixed(to[0]), fixed(to[1]), fixed(to[2]), fixed(to[3])
  );
}

void DskSurfaceClient::handleStarted(void* data, dsk_surface_v1* /*surface*/, uint32_t serial, uint32_t time) {
  auto* self = static_cast<DskSurfaceClient*>(data);
  const auto it = self->m_started.find(serial);
  if (it == self->m_started.end()) {
    return;
  }
  StartedCallback callback = std::move(it->second);
  self->m_started.erase(it);
  callback(time);
}

void DskSurfaceClient::handleClipMapped(
    void* data, dsk_surface_v1* /*surface*/, int32_t x, int32_t y, int32_t width, int32_t height
) {
  auto* self = static_cast<DskSurfaceClient*>(data);
  self->m_mappedClip =
      Value{wl_fixed_to_double(x), wl_fixed_to_double(y), wl_fixed_to_double(width), wl_fixed_to_double(height)};
  if (self->m_onClipMapped) {
    self->m_onClipMapped(*self->m_mappedClip);
  }
}

void DskSurfaceClient::handleBackdrop(void* data, dsk_surface_v1* /*surface*/, uint32_t tone) {
  auto* self = static_cast<DskSurfaceClient*>(data);
  self->m_backdropDark = tone == DSK_SURFACE_V1_BACKDROP_DARK;
  if (self->m_onBackdrop) {
    self->m_onBackdrop(*self->m_backdropDark);
  }
}

void DskSurfaceClient::handleDone(void* data, dsk_surface_v1* /*surface*/, uint32_t serial, uint32_t reason) {
  auto* self = static_cast<DskSurfaceClient*>(data);
  self->m_started.erase(serial);
  const auto it = self->m_pending.find(serial);
  if (it == self->m_pending.end()) {
    return;
  }
  DoneCallback callback = std::move(it->second);
  self->m_pending.erase(it);
  callback(static_cast<EndReason>(reason));
}

const dsk_tracker_v1_listener DskTrackerClient::kListener = {
    .interacting = handleInteracting,
    .inertia = handleInertia,
    .idle = handleIdle,
    .position = handlePosition,
};

DskTrackerClient::DskTrackerClient(dsk_surface_manager_v1* manager, wl_surface* surface, Callbacks callbacks)
    : m_tracker(dsk_surface_manager_v1_get_tracker(manager, surface)), m_callbacks(std::move(callbacks)) {
  dsk_tracker_v1_add_listener(m_tracker, &kListener, this);
}

DskTrackerClient::~DskTrackerClient() { dsk_tracker_v1_destroy(m_tracker); }

void DskTrackerClient::setSource(Axis axis, uint32_t fingers, wl_region* region) {
  dsk_tracker_v1_set_source(m_tracker, static_cast<uint32_t>(axis), fingers, region);
}

void DskTrackerClient::setRange(double minimum, double maximum) {
  dsk_tracker_v1_set_range(m_tracker, fixed(minimum), fixed(maximum));
}

void DskTrackerClient::addSnap(double position) { dsk_tracker_v1_add_snap(m_tracker, fixed(position)); }

void DskTrackerClient::bind(
    const DskSurfaceClient& target, DskSurfaceClient::Property property, uint32_t component, double p0, double p1,
    double v0, double v1
) {
  dsk_tracker_v1_bind(
      m_tracker, target.resource(), static_cast<uint32_t>(property), component, fixed(p0), fixed(p1), fixed(v0),
      fixed(v1)
  );
}

void DskTrackerClient::moveTo(double position, DskSurfaceClient::Curve curve) {
  dsk_tracker_v1_move_to(m_tracker, fixed(position),
      static_cast<uint32_t>(DskSurfaceClient::compatCurve(curve, dsk_tracker_v1_get_version(m_tracker))));
}

void DskTrackerClient::watch(bool enabled) { dsk_tracker_v1_watch(m_tracker, enabled ? 1U : 0U); }

void DskTrackerClient::handleInteracting(void* data, dsk_tracker_v1* /*tracker*/, uint32_t /*time*/) {
  if (const auto& callback = static_cast<DskTrackerClient*>(data)->m_callbacks.interacting) {
    callback();
  }
}

void DskTrackerClient::handleInertia(void* data, dsk_tracker_v1* /*tracker*/, uint32_t /*time*/, int32_t target) {
  if (const auto& callback = static_cast<DskTrackerClient*>(data)->m_callbacks.inertia) {
    callback(wl_fixed_to_double(target));
  }
}

void DskTrackerClient::handleIdle(void* data, dsk_tracker_v1* /*tracker*/, int32_t position) {
  if (const auto& callback = static_cast<DskTrackerClient*>(data)->m_callbacks.idle) {
    callback(wl_fixed_to_double(position));
  }
}

void DskTrackerClient::handlePosition(void* data, dsk_tracker_v1* /*tracker*/, uint32_t /*time*/, int32_t position) {
  if (const auto& callback = static_cast<DskTrackerClient*>(data)->m_callbacks.position) {
    callback(wl_fixed_to_double(position));
  }
}
