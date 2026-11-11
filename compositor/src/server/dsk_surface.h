#pragma once

#include "server/surface_motion.h"
#include "server/tracker_motion.h"

#include <pixman.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/addon.h>
#include <wlr/util/box.h>
}

struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_surface;
struct dsk_surface_manager_v1_interface;
struct dsk_surface_v1_interface;
struct dsk_tracker_v1_interface;
struct dsk_shape_v1_interface;

namespace andromeda {

  // Serves dsk_surface_manager_v1 (protocol/dsk-surface-v1.xml): shape, elevation and compositor-run animations of
  // our own clients' surfaces. Requests are double-buffered and apply on the surface's next commit.
  class DskSurfaceManager {
  public:
    struct State {
      SurfaceMotion motion;
      int backdropTone = -1; // last dsk_surface_v1.backdrop sent, -1 = none yet
      int version = 0; // dsk_surface_v1 version the client bound
      // dsk_surface_v1.set_visible_region (v8), surface-local; nullopt = the whole surface.
      std::optional<std::vector<pixman_box32_t>> visibleRegion;
      // dsk_shape_v1 (v10) glass shapes in creation order; each owns its properties and animations.
      std::vector<SurfaceMotion*> shapes;
      // dsk_surface_v1.set_glass_mask (v13): the locked mask buffer and its surface-local box; null = none.
      // glassMaskSerial changes with every committed mask, so a consumer rebuilds its mask node only then.
      wlr_buffer* glassMask = nullptr;
      wlr_box glassMaskBox{};
      uint32_t glassMaskSerial = 0;
      bool secure = false; // dsk_surface_v1.set_secure (v14)
    };

    explicit DskSurfaceManager(wl_display* display);
    ~DskSurfaceManager();

    DskSurfaceManager(const DskSurfaceManager&) = delete;
    DskSurfaceManager& operator=(const DskSurfaceManager&) = delete;

    // Layout position of a layer surface (mapped or about to map), nullopt otherwise; set_clip_from does nothing until the server sets it.
    void setOriginLookup(std::function<std::optional<std::pair<int, int>>(wlr_surface*)> lookup) {
      m_originLookup = std::move(lookup);
    }

    // dsk_surface_v1.launch_from (v11): `box` is surface-local.
    void setLaunchFromHandler(std::function<void(wl_client*, wlr_surface*, const wlr_box&)> handler) {
      m_launchFromHandler = std::move(handler);
    }

    // Called when a surface's committed secure flag changes, and with false when its dsk_surface_v1 goes while the
    // surface lives.
    void setSecureHandler(std::function<void(wlr_surface*, bool)> handler) { m_secureHandler = std::move(handler); }
    // True while any surface is secure or a secure copy lives; a cheap gate for per-frame capture redaction.
    [[nodiscard]] bool anySecure() const { return m_secureCount > 0 || m_fixedCopies > 0; }
    // True when `surface` itself is secure, or its dsk_surface_v1 went while secure and no later commit has applied.
    [[nodiscard]] bool secure(wlr_surface* surface) const;
    // True when `surface` or a surface it is a subsurface or xdg_popup of is secure.
    [[nodiscard]] bool secureTree(wlr_surface* surface) const;
    // Output captures paint `copy`, a scene buffer that shows a secure surface's buffer (a thumbnail, a close
    // snapshot), black until the node is destroyed.
    void markSecureCopy(wlr_scene_buffer* copy);
    // Output captures paint `copy`, a scene buffer that follows `source`'s buffers, black while secureTree(source);
    // when `source` goes first, the copy keeps the state `source` had.
    void markMirror(wlr_scene_buffer* copy, wlr_surface* source);
    [[nodiscard]] bool secureCopy(const wlr_scene_buffer* buffer) const;

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
      enum class Kind : uint8_t { Set, Animate, ClipFrom, Loop } kind;
      uint32_t serial;
      SurfaceMotion::Property property;
      SurfaceMotion::Curve curve;
      SurfaceMotion::Value value;
      SurfaceMotion::LoopSpec loop; // Loop only
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

    // A dsk_shape_v1; its ops apply on the parent wl_surface commit. parent is null once the dsk_surface_v1 is gone.
    struct Shape {
      DskSurfaceManager* manager = nullptr;
      wl_resource* resource = nullptr;
      Surface* parent = nullptr;
      std::vector<Op> pending{};
      SurfaceMotion motion{};
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
      // A set_glass_mask since the last commit: the locked buffer (null = remove) and its origin.
      bool pendingGlassMaskSet = false;
      wlr_buffer* pendingGlassMask = nullptr;
      wlr_box pendingGlassMaskBox{};
      std::optional<bool> pendingSecure{};
      // The wl_surface state seq whose application clears current.secure; a cached commit (sync subsurface, fifo,
      // syncobj) applies later, and its old buffer stays on screen until then.
      std::optional<uint32_t> secureClearSeq{};
      wl_listener commit{};
      State current{};
      std::vector<Shape*> shapes{}; // every live dsk_shape_v1, in creation order
    };

    static const struct dsk_surface_manager_v1_interface kManagerImplementation;
    static const struct dsk_shape_v1_interface kShapeImplementation;
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
    static void handleLaunchFrom(wl_client* client, wl_resource* resource, int32_t x, int32_t y, int32_t width, int32_t height);
    static void handleSetVisibleRegion(wl_client* client, wl_resource* resource, wl_resource* region);
    static void handleSetGlassMask(
        wl_client* client, wl_resource* resource, wl_resource* buffer, int32_t x, int32_t y, int32_t width,
        int32_t height
    );
    static void handleSetSecure(wl_client* client, wl_resource* resource, uint32_t secure);
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
    static void handleGetShape(wl_client* client, wl_resource* resource, uint32_t id);
    static Shape* shapeFromResource(wl_resource* resource);
    static void handleShapeDestroy(wl_client* client, wl_resource* resource);
    static void handleShapeSet(
        wl_client* client, wl_resource* resource, uint32_t property, int32_t a, int32_t b, int32_t c, int32_t d
    );
    static void handleShapeAnimate(
        wl_client* client, wl_resource* resource, uint32_t serial, uint32_t property, uint32_t curve, int32_t a,
        int32_t b, int32_t c, int32_t d
    );
    static void handleShapeLoop(
        wl_client* client, wl_resource* resource, uint32_t serial, uint32_t property, uint32_t wave,
        uint32_t periodMs, uint32_t seed, int32_t a, int32_t b, int32_t c, int32_t d
    );
    static void handleShapeResourceDestroyed(wl_resource* resource);
    // Applies `ops` to `motion` as a commit does.
    static void applyOps(const std::vector<Op>& ops, SurfaceMotion& motion);
    static void sendShapeEvents(Shape& shape, uint32_t timeMs);
    static void handleClientCommit(wl_listener* listener, void* data);
    static void handleCommit(wl_listener* listener, void* data);
    static void setSecure(Surface* state, bool secure);
    static void handleAddonDestroy(wlr_addon* addon);
    // timeMs stamps started events: the frame time from tick(), or the commit time.
    static void sendEvents(Surface& state, uint32_t timeMs);
    // `surfaceAlive`: the wl_surface outlives the state, so the secure handler may still see it.
    static void destroySurfaceState(Surface* state, bool surfaceAlive);
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
    std::function<void(wl_client*, wlr_surface*, const wlr_box&)> m_launchFromHandler;
    std::function<void(wlr_surface*, bool)> m_secureHandler;
    size_t m_secureCount = 0;
    struct SecureCopy {
      enum class Kind : uint8_t { Fixed, Mirror };
      DskSurfaceManager* manager;
      wlr_scene_buffer* buffer;
      Kind kind;
      wlr_surface* source; // valid while kind is Mirror
      wl_listener destroy;
      wl_listener sourceDestroy;
    };
    void addCopy(wlr_scene_buffer* copy, wlr_surface* source);
    // A secure surface whose dsk_surface_v1 went: it stays secure until the state of its next commit applies.
    struct SecureTail {
      DskSurfaceManager* manager;
      wlr_surface* surface;
      uint32_t seq;
      wl_listener commit;
      wl_listener destroy;
    };
    void addSecureTail(wlr_surface* surface);
    void endSecureTail(SecureTail* tail, bool surfaceAlive);
    std::vector<std::unique_ptr<SecureTail>> m_secureTails;
    std::vector<std::unique_ptr<SecureCopy>> m_secureCopies;
    size_t m_fixedCopies = 0;
  };

} // namespace andromeda
