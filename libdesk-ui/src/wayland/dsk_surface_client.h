#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

struct wl_surface;
struct dsk_surface_manager_v1;
struct dsk_surface_v1;
struct dsk_tracker_v1;
struct wl_region;
struct dsk_surface_v1_listener;
struct dsk_tracker_v1_listener;

// Client side of dsk_surface_v1 (protocol/dsk-surface-v1.xml): the compositor draws the surface's elevation shadow and
// runs its animations and gestures. Requests apply on the surface's next wl_surface.commit. Exists only on our
// compositor: create it when WaylandConnection::dskSurfaceManager() is not null.
class DskSurfaceClient {
public:
  // Values match the protocol enums.
  enum class Property : uint32_t { Translate, Scale, Opacity, Clip, Radius, Elevation };
  enum class Curve : uint32_t { None, SpringStandard, SpringSnappy, SpringBouncy, EaseFast, EaseNormal, EaseSlow, SpringOpen, SpringClose, SpringIslandExpand, SpringIslandCollapse, SpringPage, SpringPress, SpringRelease, SpringEffects, SpringEffectsFast };
  // A compositor that binds protocol version < 3 knows only the first seven curves: use the closest one.
  [[nodiscard]] static Curve compatCurve(Curve curve, uint32_t version);
  enum class Elevation : uint32_t { None, E1, E2 };
  enum class EndReason : uint32_t { Finished, Retargeted, Cancelled };
  using Value = std::array<double, 4>;
  using DoneCallback = std::function<void(EndReason)>;
  // CLOCK_MONOTONIC ms of the animation's first presented frame; the curve and start value give every later value.
  using StartedCallback = std::function<void(uint32_t timeMs)>;

  DskSurfaceClient(dsk_surface_manager_v1* manager, wl_surface* surface);
  ~DskSurfaceClient();

  DskSurfaceClient(const DskSurfaceClient&) = delete;
  DskSurfaceClient& operator=(const DskSurfaceClient&) = delete;

  void setElevation(Elevation level);
  void set(Property property, const Value& value);
  // Runs from the property's current value; `onDone` fires once when it settles, is retargeted or is cancelled.
  void animate(Property property, Curve curve, const Value& to, DoneCallback onDone = {}, StartedCallback onStarted = {});
  // Protocol version 4: the shadow level (0 none, 1 e1, 2 e2, fractions between) is a property that set and animate move.
  [[nodiscard]] bool hasElevationProperty() const;
  // The curve the compositor runs for `curve` on this connection's protocol version.
  [[nodiscard]] Curve effectiveCurve(Curve curve) const;
  // Sets Clip to `rect` (x, y, width, height) of `source`, another layer surface of this client, mapped by the
  // compositor into this surface's coordinates. False when the compositor lacks it (protocol version 1).
  bool setClipFrom(wl_surface* source, const Value& rect);
  // The clip the last setClipFrom computed, in this surface's coordinates; empty until the compositor reports it.
  [[nodiscard]] const std::optional<Value>& mappedClip() const noexcept { return m_mappedClip; }
  // Runs when the compositor reports the clip of the last setClipFrom.
  void setOnClipMapped(std::function<void(const Value&)> callback) { m_onClipMapped = std::move(callback); }

  [[nodiscard]] dsk_surface_v1* resource() const noexcept { return m_surface; }

private:
  static const dsk_surface_v1_listener kListener;
  static void handleStarted(void* data, dsk_surface_v1* surface, uint32_t serial, uint32_t time);
  static void handleDone(void* data, dsk_surface_v1* surface, uint32_t serial, uint32_t reason);
  static void handleClipMapped(void* data, dsk_surface_v1* surface, int32_t x, int32_t y, int32_t width, int32_t height);

  dsk_surface_v1* m_surface = nullptr;
  uint32_t m_nextSerial = 1;
  std::optional<Value> m_mappedClip;
  std::function<void(const Value&)> m_onClipMapped;
  std::unordered_map<uint32_t, DoneCallback> m_pending;
  std::unordered_map<uint32_t, StartedCallback> m_started;
};

// A compositor-owned gesture on a surface: the compositor tracks the input, runs inertia to a snap, and writes the
// position into bound properties without waking the client per frame.
class DskTrackerClient {
public:
  enum class Axis : uint32_t { Vertical, Horizontal, Pinch };
  struct Callbacks {
    std::function<void()> interacting;
    std::function<void(double target)> inertia;
    std::function<void(double position)> idle;
    // Only after watch(true): per-frame position, for client pixels that must follow the finger.
    std::function<void(double position)> position;
  };

  DskTrackerClient(dsk_surface_manager_v1* manager, wl_surface* surface, Callbacks callbacks);
  ~DskTrackerClient();

  DskTrackerClient(const DskTrackerClient&) = delete;
  DskTrackerClient& operator=(const DskTrackerClient&) = delete;

  // fingers: touchpad finger count; 0 is a pointer or touch drag. region: nullptr for the whole surface.
  void setSource(Axis axis, uint32_t fingers, wl_region* region);
  void setRange(double minimum, double maximum);
  void addSnap(double position);
  // Maps position p0..p1 to v0..v1 on one component (0..3) of a property of `target`.
  void bind(const DskSurfaceClient& target, DskSurfaceClient::Property property, uint32_t component, double p0,
            double p1, double v0, double v1);
  void moveTo(double position, DskSurfaceClient::Curve curve);
  void watch(bool enabled);

private:
  static const dsk_tracker_v1_listener kListener;
  static void handleInteracting(void* data, dsk_tracker_v1* tracker, uint32_t time);
  static void handleInertia(void* data, dsk_tracker_v1* tracker, uint32_t time, int32_t target);
  static void handleIdle(void* data, dsk_tracker_v1* tracker, int32_t position);
  static void handlePosition(void* data, dsk_tracker_v1* tracker, uint32_t time, int32_t position);

  dsk_tracker_v1* m_tracker = nullptr;
  Callbacks m_callbacks;
};
