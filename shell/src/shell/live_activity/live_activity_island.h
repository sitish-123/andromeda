#pragma once

#include "pipewire/privacy_filter.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/bar/center_island_space.h"
#include "shell/osd/osd_overlay.h"
#include "wayland/layer_surface.h"

#include <array>
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
    float width = 0.0F; // surface width: the widest card
    CenterIslandSpace space;
    float liveWidth = 0.0F; // widest card the free center space allows
    float restWidth = 0.0F;
    std::unique_ptr<LayerSurface> surface;
    // Our compositor morphs the island (clip and radius) and draws its shadow; null on other compositors.
    std::unique_ptr<DskSurfaceClient> dsk;
    // sceneRoot must be destroyed before `animations`: ~Node() cancels its animations.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher; // after sceneRoot: it points into the scene
    bool pointerInside = false;
    Box* shadow = nullptr; // glass drop shadow of the rest pill
    Box* edge = nullptr;   // glass top edge highlight of the rest pill
    float pad = 0.0F;      // surface rows above the island, for the shadow bleed
    Box* material = nullptr;
    Box* tint = nullptr;
    Box* ring = nullptr;
    InputArea* restArea = nullptr;
    std::array<Node*, kFaceCount> faceNode{};
    std::array<float, kFaceCount> faceWidth{};
    Box* micDot = nullptr;
    Box* cameraDot = nullptr;
    Box* screenDot = nullptr;
    Node* mediaGroup = nullptr;
    Node* artPlaceholder = nullptr;
    Image* art = nullptr;
    Node* bars = nullptr;
    std::array<Box*, 3> barBox{};
    std::array<float, 3> barLevel{};
    Label* date = nullptr;
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
    Label* osdValue = nullptr;
    Glyph* bannerGlyph = nullptr;
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
    Rect from;
    Rect to;
    Rect cur;
    bool shapeInit = false;
    std::optional<Rect> dskTarget;
  };

  struct BannerItem {
    std::uint32_t id = 0;
    std::string title;
    std::string body;
    bool defaultAction = false;
  };

  void refreshNow();
  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, float width, float height);
  void applyContent(Instance& inst);
  void applyReveal(Instance& inst, float reveal);
  void animateReveal(Instance& inst, float velocity);
  void updateFace(Instance& inst);
  void updateAllFaces();
  [[nodiscard]] Face desiredFace(const Instance& inst) const;
  void syncDsk(Instance& inst, bool animate);
  [[nodiscard]] float surfaceHeight() const;
  [[nodiscard]] static std::int32_t surfaceMargin(const CenterIslandSpace& space);
  [[nodiscard]] Rect faceRect(const Instance& inst, Face face) const;
  void hoverEnter(Instance& inst);
  void hoverLeave(Instance& inst);
  void loadArt(Instance& inst, Renderer& renderer, const std::string& artUrl);
  void syncBars();
  void tickBars();
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
};
