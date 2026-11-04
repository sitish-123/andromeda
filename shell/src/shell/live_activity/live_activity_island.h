#pragma once

#include "pipewire/privacy_filter.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "notification/notification.h"
#include "shell/bar/center_island_space.h"
#include "system/icon_resolver.h"
#include "shell/osd/osd_overlay.h"
#include "wayland/layer_surface.h"
#include "ui/signal.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class Box;
class Button;
class CalendarService;
class HttpClient;
class Image;
struct Notification;
enum class NotificationEvent;
class PipeWireSpectrum;
class DskSurfaceClient;
class ConfigService;
class Glyph;
class InputArea;
class NotificationManager;
class MprisService;
class PipeWireService;
class Label;
class Node;
class ProgressBar;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct wl_output;

// One running task shown in the top-center island: a file transfer, a call, a recording, or media.
struct LiveActivity {
  std::string glyph;
  std::string title;
  std::string subtitle;
  // 0..1; no bar when empty.
  std::optional<float> progress;
  std::string actionGlyph;
  std::function<void()> action;
  bool actionDestructive = false; // danger fill, for example decline
  // Shown before the main action, for example mute before decline.
  std::string secondaryGlyph;
  std::function<void()> secondaryAction;
  // A click on the island body; unset ignores it.
  std::function<void(wl_output*)> open;
  // Set for an answered call: the island rests in the bar row with the glyph and the elapsed time; hover shows the card.
  std::optional<std::chrono::steady_clock::time_point> since;
};

// The center island of the top or bottom islands bar. At rest it is a pill in the bar row with the privacy dots and the
// unread notification count. A live activity grows it away from the bar edge, over windows, with a shadow and tint.
class LiveActivityIsland {
public:
  using VisibilityCallback = std::function<void(wl_output*, bool)>;
  using SpaceProvider = std::function<std::optional<CenterIslandSpace>(wl_output*)>;

  LiveActivityIsland();
  ~LiveActivityIsland();

  LiveActivityIsland(const LiveActivityIsland&) = delete;
  LiveActivityIsland& operator=(const LiveActivityIsland&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  // Runs per output with true when its island shows and false when it is gone, so the bar can hide that center section.
  void setVisibilityCallback(VisibilityCallback callback);
  void setSpaceProvider(SpaceProvider provider);
  // Sources of the rest content, and the click on the rest pill.
  void setRestSources(
      std::function<PipeWireService*()> pipewire, std::function<MprisService*()> mpris, NotificationManager* notifications,
      std::function<void(wl_output*)> open
  );
  // Album art download, audio levels for the media bars, and the next event of the idle hover face.
  void setExtraSources(HttpClient* http, std::function<PipeWireSpectrum*()> spectrum, CalendarService* calendar);
  // Date text at rest changes at midnight; the hover clock changes every minute.
  void onTimeTick();
  // The island takes the OSD or the banner and returns true; false when the caller must show its own surface.
  [[nodiscard]] bool showOsd(const OsdContent& content);
  [[nodiscard]] bool osdVisible() const;
  [[nodiscard]] bool showBanner(const Notification& notification, NotificationEvent event);
  void setActivity(std::optional<LiveActivity> activity);
  // A fullscreen window: the rest pill hides under it; other faces and an active call still show over it.
  void setFullscreen(bool fullscreen);
  // IPC: "none" or "title|subtitle|secondary glyph|primary glyph"; press runs the button's action.
  [[nodiscard]] std::string describeActivity() const;
  bool pressActivityButton(bool secondary);
  // The bar layout, the config, the privacy state or the unread count changed.
  void refresh();
  // A callable that refreshes while this island lives and does nothing after.
  [[nodiscard]] std::function<void()> refresher();
  void onOutputChange();
  bool onPointerEvent(const PointerEvent& event);

private:
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
    float radius = 0.0F;
  };

  enum class Face : std::uint8_t { Rest, HoverMedia, HoverIdle, Osd, Banner, Live };
  static constexpr std::size_t kFaceCount = 6;

  struct Instance {
    wl_output* output = nullptr;
    // Theme, palette and glass-slider changes re-apply the material, as on the bar and dock.
    Signal<>::ScopedConnection paletteConn;
    float width = 0.0F; // surface width: the widest card
    CenterIslandSpace space;
    float liveWidth = 0.0F; // widest card the free center space allows
    float restWidth = 0.0F;
    float scale = 1.0F; // surface px per layout px
    std::unique_ptr<LayerSurface> surface;
    // Our compositor morphs the island (clip and radius) and draws its shadow; null on other compositors.
    std::unique_ptr<DskSurfaceClient> dsk;
    std::optional<bool> backdropDark;
    // sceneRoot must be destroyed before `animations`: ~Node() cancels its animations.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher; // after sceneRoot: it points into the scene
    bool pointerInside = false;
    Box* shadow = nullptr; // glass drop shadow of the rest pill
    float pad = 0.0F;      // surface rows above the island, for the shadow bleed
    Box* material = nullptr;
    Box* tint = nullptr;
    Box* ring = nullptr;
    InputArea* restArea = nullptr;
    std::array<Node*, kFaceCount> faceNode{};
    std::array<float, kFaceCount> faceWidth{};
    std::array<float, kFaceCount> faceHeight{};
    Box* micDot = nullptr;
    Box* cameraDot = nullptr;
    Box* screenDot = nullptr;
    Node* mediaGroup = nullptr;
    Label* mediaTitle = nullptr;
    Node* artPlaceholder = nullptr;
    Image* art = nullptr;
    Node* bars = nullptr;
    std::array<Box*, 3> barBox{};
    std::array<float, 3> barLevel{};
    Label* date = nullptr;
    Glyph* callGlyph = nullptr;
    Box* divider = nullptr;
    Label* count = nullptr;
    Glyph* icon = nullptr;
    Label* title = nullptr;
    Label* subtitle = nullptr;
    ProgressBar* progress = nullptr;
    Button* action = nullptr;
    Button* secondary = nullptr;
    Image* hoverArt = nullptr;
    Node* hoverArtPlaceholder = nullptr;
    Label* hoverTitle = nullptr;
    Label* hoverArtist = nullptr;
    Button* prev = nullptr;
    Button* playPause = nullptr;
    Button* next = nullptr;
    Label* idleTime = nullptr;
    Label* idleDate = nullptr;
    Label* idleEvent = nullptr;
    Glyph* osdGlyph = nullptr;
    ProgressBar* osdBar = nullptr;
    ProgressBar* hoverProgress = nullptr;
    Label* osdValue = nullptr;
    Glyph* bannerGlyph = nullptr;
    Node* bannerCircle = nullptr;
    Image* bannerImage = nullptr;
    std::string bannerIconKey; // banner id, icon and entry the icon node shows
    Label* bannerApp = nullptr;
    Label* bannerTime = nullptr;
    Label* bannerTitle = nullptr;
    Label* bannerBody = nullptr;
    std::string artUrl;
    std::string hoverArtUrl;
    Face face = Face::Rest;
    Face prevFace = Face::Rest;
    bool hovered = false;
    bool hoverMedia = false; // latched when the hover starts, so a pause does not swap the face under the pointer
    Timer collapse;
    float reveal = 1.0F; // 0..1 from `from` to `to`
    AnimationManager::Id revealAnimId = 0;
    float osdProgress = 0.0F;
    float dragOffset = 0.0F; // banner swipe: surface translate, px (negative = up)
    float dragStartY = 0.0F;
    float dragLastY = 0.0F;
    float dragVelocity = 0.0F; // px/ms
    std::chrono::steady_clock::time_point dragLastAt;
    desk::motion::Spring dragSpring;
    Timer dragTimer;
    bool dragging = false;
    bool dragMoved = false;
    bool dragDismiss = false;
    std::uint32_t dragBannerId = 0;
    AnimationManager::Id osdAnimId = 0;
    Rect from;
    Rect to;
    Rect cur;
    bool shapeInit = false;
    std::optional<Rect> dskTarget;
    // Mirror of the compositor's clip and radius springs (x, y, w, h, radius), started by the `started` event, so
    // `cur`, the face fades, the blur and the input region follow the clip frame for frame. Off without a compositor
    // or with reduced motion.
    struct Mirror {
      std::array<desk::motion::Spring, 5> shape;
      desk::motion::Spring fade; // 0..1 from the last face change
      bool active = false;
      bool waiting = false;     // requested, no `started` yet: values hold
      bool fadePending = false; // the next `started` also starts `fade`
      bool retarget = false;    // the compositor keeps a running spring: its first tick already steps one frame
      std::uint32_t generation = 0;
      std::uint32_t lastStepMs = 0;
      std::uint32_t frameMs = 16; // last measured step interval
      std::uint32_t startMs = 0;
      std::uint32_t fadeStartMs = 0;
      double shapeSec = 0.0;
      double fadeSec = 0.0;
      std::chrono::steady_clock::time_point requested;
    } mirror;
    Timer mirrorStop;
    double dskLevel = 0.0; // last elevation level sent (protocol version 4)
  };

  struct BannerItem {
    std::uint32_t id = 0;
    std::string title;
    std::string body;
    bool defaultAction = false;
    std::string appName;
    std::string time;
    std::string icon;
    std::string desktopEntry;
    std::optional<NotificationImageData> imageData;
  };

  void refreshNow();
  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, float width, float height);
  void applyContent(Instance& inst);
  void applyReveal(Instance& inst, float reveal);
  void animateReveal(Instance& inst, float velocity);
  void bannerDragStart(Instance& inst, float y);
  void bannerDragMove(Instance& inst, float y);
  void bannerDragEnd(Instance& inst);
  void startBannerSpring(Instance& inst, bool dismiss);
  [[nodiscard]] bool bannerDragStale(const Instance& inst) const;
  void finishBannerDrag(Instance& inst, bool dismiss);
  void applyBannerOffset(Instance& inst, float offset);
  [[nodiscard]] float bannerMinOffset(const Instance& inst) const;
  void updateFace(Instance& inst);
  void applyLayer(Instance& inst);
  void updateAllFaces();
  [[nodiscard]] Face desiredFace(const Instance& inst) const;
  void syncDsk(Instance& inst, bool animate);
  void startMirror(Instance& inst, const Rect& target, bool grow, bool waitForStart);
  void stepMirror(Instance& inst);
  void ensureMirrorDriver(Instance& inst);
  [[nodiscard]] static bool mirrored(const Instance& inst);
  void finishReveal(Instance& inst);
  [[nodiscard]] float surfaceHeight(float scale) const;
  [[nodiscard]] static std::int32_t surfaceMargin(const CenterIslandSpace& space);
  [[nodiscard]] Rect faceRect(const Instance& inst, Face face) const;
  void hoverEnter(Instance& inst);
  void hoverLeave(Instance& inst);
  void loadArt(Instance& inst, Renderer& renderer, const std::string& artUrl);
  void syncBars();
  void tickBars();
  void syncHoverProgress(Instance& inst);
  void startNextBanner();
  void endBanner();
  void syncVisibility();
  [[nodiscard]] bool coversAllOutputs() const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  VisibilityCallback m_visibilityCallback;
  SpaceProvider m_spaceProvider;
  std::function<PipeWireService*()> m_pipewire;
  std::function<MprisService*()> m_mpris;
  std::string m_date;
  NotificationManager* m_notifications = nullptr;
  std::function<void(wl_output*)> m_open;
  PrivacyFilter m_micFilter;
  PrivacyFilter m_camFilter;
  PrivacyFilter m_screenFilter;
  HttpClient* m_http = nullptr;
  std::function<PipeWireSpectrum*()> m_spectrum;
  CalendarService* m_calendar = nullptr;
  std::uint64_t m_spectrumListener = 0;
  std::unordered_set<std::string> m_pendingArt;
  std::string m_minute;
  Timer m_barsTimer;
  float m_barsPhase = 0.0F;
  Timer m_osdTimer;
  std::optional<OsdContent> m_osd;
  Timer m_bannerTimer;
  Timer m_callTimer;
  IconResolver m_iconResolver;
  std::optional<BannerItem> m_banner;
  std::vector<BannerItem> m_bannerQueue;
  // Ids the island showed as a banner: their later updates never raise a toast.
  std::unordered_set<std::uint32_t> m_consumed;
  std::optional<LiveActivity> m_activity;
  // Kept while the island shrinks after the activity ended.
  LiveActivity m_shown;
  std::unordered_set<wl_output*> m_visibleOutputs;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  bool m_refreshScheduled = false;
  std::vector<std::unique_ptr<Instance>> m_instances;
  bool m_fullscreen = false;
};
