#pragma once

#include "server/surface_motion.h"
#include "server/tracker_motion.h"

#include <pixman.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/addon.h>
}

struct wlr_surface;
struct dsk_surface_manager_v1_interface;
struct dsk_surface_v1_interface;
struct dsk_tracker_v1_interface;

namespace andromeda {

  // Serves dsk_surface_manager_v1 (protocol/dsk-surface-v1.xml): shape, elevation and compositor-run animations of
  // our own clients' surfaces. Requests are double-buffered and apply on the surface's next commit.
  class DskSurfaceManager {
  public:
    struct State {
      SurfaceMotion motion;
      int backdropTone = -1; // last dsk_surface_v1.backdrop sent, -1 = none yet
      // dsk_surface_v1.set_visible_region (v8), surface-local; nullopt = the whole surface.
      std::optional<std::vector<pixman_box32_t>> visibleRegion;
    };

    explicit DskSurfaceManager(wl_display* display);
    ~DskSurfaceManager();

    DskSurfaceManager(const DskSurfaceManager&) = delete;
    DskSurfaceManager& operator=(const DskSurfaceManager&) = delete;

    // Layout position of a layer surface (mapped or about to map), nullopt otherwise; set_clip_from does nothing until the server sets it.
    void setOriginLookup(std::function<std::optional<std::pair<int, int>>(wlr_surface*)> lookup) {
      m_originLookup = std::move(lookup);
    }

    // The committed state of `surface`; nullptr when it has no dsk_surface_v1.
    [[nodiscard]] State* state(wlr_surface* surface) const;
    // Advances the surface's animations to `nowMs` and sends their started and done events. Returns true when a
    // value changed.
    // Also advances the trackers of `surface` and writes their bound values.
    bool tick(wlr_surface* surface, uint32_t nowMs) const;
    // Sends dsk_surface_v1.backdrop (v6) when `dark` differs from the tone the client last got.
    void sendBackdrop(wlr_surface* surface, bool dark) const;
    // True while a tracker drives a property of `surface` (interacting or inertia).
    [[nodiscard]] bool driven(wlr_surface* surface) const;

    // Touchpad swipe routing from Gestures. beginSwipe claims the gesture when a tracker of `surface` takes `fingers`
    // and its source region holds the surface-local point (sx, sy); the other calls feed the claimed tracker.
    // fingers 0 is a pointer or touch drag; the caller claims it only after the drag moved past its threshold.
    bool beginSwipe(wlr_surface* surface, double sx, double sy, uint32_t fingers, uint32_t timeMs);
    // The axis of a drag tracker (fingers 0) at the surface-local point: true horizontal, false vertical, nullopt none.
    [[nodiscard]] std::optional<bool> dragAxisHorizontal(wlr_surface* surface, double sx, double sy) const;
    void updateSwipe(double dx, double dy, uint32_t timeMs);
    void endSwipe(uint32_t timeMs);
    // Pinch trackers: endSwipe() and cancelSwipe() end a pinch too.
    bool beginPinch(wlr_surface* surface, double sx, double sy, uint32_t fingers, uint32_t timeMs);
    void updatePinch(double scale, uint32_t timeMs);
    // Settles a claimed swipe that ends without an input event, for example when the session locks.
    void cancelSwipe();

  private:
    struct Op {
      // ClipFrom: value is a clip in layout coordinates, made surface-local at commit.
      enum class Kind : uint8_t { Set, Animate, ClipFrom } kind;
      uint32_t serial;
      SurfaceMotion::Property property;
      SurfaceMotion::Curve curve;
      SurfaceMotion::Value value;
    };

    struct Surface;

    struct Binding {
      Surface* target = nullptr;
      SurfaceMotion::Property property = SurfaceMotion::Property::Translate;
      uint32_t component = 0;
      double p0 = 0.0;
      double p1 = 0.0;
      double v0 = 0.0;
      double v1 = 0.0;
    };

    // Double-buffered on the tracker's wl_surface commit.
    struct TrackerConfig {
      uint32_t axis = 0;
      uint32_t fingers = 0;
      std::vector<pixman_box32_t> region{}; // empty = the whole surface
      double minimum = 0.0;
      double maximum = 0.0;
      std::vector<double> snaps{};
      std::vector<Binding> bindings{};
      bool watch = false;
    };

    struct Tracker {
      DskSurfaceManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wl_listener clientCommit{};
      wl_listener surfaceDestroy{};
      TrackerConfig pending{};
      TrackerConfig current{};
      bool pendingChanged = false;
      std::optional<std::pair<double, SurfaceMotion::Curve>> pendingMove{};
      TrackerMotion motion{};
    };

    // Lives with its wl_surface while the protocol object exists.
    struct Surface {
      DskSurfaceManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_surface* surface = nullptr;
      wlr_addon addon{};
      wl_listener clientCommit{};
      std::vector<Op> pending{};
      std::optional<std::optional<std::vector<pixman_box32_t>>> pendingVisibleRegion{};
      State current{};
    };

    static const struct dsk_surface_manager_v1_interface kManagerImplementation;
    static const struct dsk_surface_v1_interface kSurfaceImplementation;
    static const struct dsk_tracker_v1_interface kTrackerImplementation;
    static const wlr_addon_interface kAddonInterface;

    static Surface* fromResource(wl_resource* resource);
    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleManagerDestroy(wl_client* client, wl_resource* resource);
    static void handleGetSurface(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface);
    static void handleGetTracker(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface);
    static void handleSurfaceDestroy(wl_client* client, wl_resource* resource);
    static void handleSetElevation(wl_client* client, wl_resource* resource, uint32_t level);
    static void handleSetVisibleRegion(wl_client* client, wl_resource* resource, wl_resource* region);
    static void
    handleSet(wl_client* client, wl_resource* resource, uint32_t property, int32_t a, int32_t b, int32_t c, int32_t d);
    static void handleAnimate(
        wl_client* client, wl_resource* resource, uint32_t serial, uint32_t property, uint32_t curve, int32_t a,
        int32_t b, int32_t c, int32_t d
    );
    static void handleSetClipFrom(
        wl_client* client, wl_resource* resource, wl_resource* source, int32_t x, int32_t y, int32_t width,
        int32_t height
    );
    static void handleSurfaceResourceDestroyed(wl_resource* resource);
    static void handleClientCommit(wl_listener* listener, void* data);
    static void handleAddonDestroy(wlr_addon* addon);
    // timeMs stamps started events: the frame time from tick(), or the commit time.
    static void sendEvents(Surface& state, uint32_t timeMs);
    static void destroySurfaceState(Surface* state);
    // The tracker sits on `surface` or writes a property of it.
    [[nodiscard]] static bool tracks(const Tracker& tracker, wlr_surface* surface);

    static Tracker* trackerFromResource(wl_resource* resource);
    static void handleTrackerDestroy(wl_client* client, wl_resource* resource);
    static void handleSetSource(wl_client* client, wl_resource* resource, uint32_t axis, uint32_t fingers, wl_resource* region);
    static void handleSetRange(wl_client* client, wl_resource* resource, int32_t minimum, int32_t maximum);
    static void handleAddSnap(wl_client* client, wl_resource* resource, int32_t position);
    static void handleBind(
        wl_client* client, wl_resource* resource, wl_resource* target, uint32_t property, uint32_t component, int32_t p0,
        int32_t p1, int32_t v0, int32_t v1
    );
    static void handleMoveTo(wl_client* client, wl_resource* resource, int32_t position, uint32_t curve);
    static void handleWatch(wl_client* client, wl_resource* resource, uint32_t enabled);
    static void handleTrackerResourceDestroyed(wl_resource* resource);
    static void handleTrackerCommit(wl_listener* listener, void* data);
    static void handleTrackerSurfaceDestroy(wl_listener* listener, void* data);
    // Writes the tracker position into its bound properties and sends its phase events.
    void applyTracker(Tracker& tracker, uint32_t timeMs) const;
    [[nodiscard]] Tracker* findTracker(wlr_surface* surface, double sx, double sy, uint32_t fingers) const;

    std::vector<Tracker*> m_trackers;
    Tracker* m_swipe = nullptr; // the tracker that owns the running swipe, drag or pinch
    double m_pinchScale = 1.0;

    wl_global* m_global = nullptr;
    std::function<std::optional<std::pair<int, int>>(wlr_surface*)> m_originLookup;
  };

} // namespace andromeda
