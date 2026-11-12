#pragma once

#include "pipewire/privacy_filter.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "notification/notification.h"
#include "shell/bar/center_island_space.h"
#include "shell/live_activity/island_banner_swipe.h"
#include "shell/live_activity/island_morph_driver.h"
#include "shell/live_activity/island_queue.h"
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
#include <string_view>
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
class DskShapeClient;
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
struct wl_surface;

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
  // Set for an answered call or a running recording: the island rests with the glyph and the elapsed time; hover shows
  // the card.
  std::optional<std::chrono::steady_clock::time_point> since;
  // The rest face of a timed activity: Call rests in every bar; Recording rests only in a bar lane, a row keeps its card.
  enum class Compact : std::uint8_t { Call, Recording };
  Compact compact = Compact::Call;
  // Shows over a fullscreen window: a ringing call.
  bool overFullscreen = false;
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
  // The lane pill a sheet grows out of: its surface, its x, y, width, height there, and its centre on the output.
  struct SheetSource {
    wl_surface* surface = nullptr;
    std::array<float, 4> rect{};
    float anchorX = 0.0F;
    float anchorY = 0.0F;
  };
  using OpenCallback = std::function<void(wl_output*, std::string_view, std::optional<SheetSource>)>;
  // False while hover cards must stay closed: a sheet is open beside the island.
  void setHoverGate(std::function<bool()> gate) { m_hoverGate = std::move(gate); }
  // Sources of the rest content, and the click on the rest pill.
  void setRestSources(
      std::function<PipeWireService*()> pipewire, std::function<MprisService*()> mpris, NotificationManager* notifications,
      OpenCallback open
  );
  // Album art download, audio levels for the media bars, and the next event of the idle hover face.
  void setExtraSources(HttpClient* http, std::function<PipeWireSpectrum*()> spectrum, CalendarService* calendar);
  // Date text at rest changes at midnight; the hover clock changes every minute.
  void onTimeTick();
  // The island takes the OSD or the banner and returns true; false when the caller must show its own surface.
  [[nodiscard]] bool showOsd(const OsdContent& content);
  [[nodiscard]] bool osdVisible() const;
  [[nodiscard]] bool showBanner(const Notification& notification, NotificationEvent event);
  // Do Not Disturb turned on: drops the banners that respect it.
  void hideDndSuppressed();
  void setActivity(std::optional<LiveActivity> activity);
  // Outputs that show a fullscreen window. There the island rests under the window and shows only a ringing call
  // (LiveActivity::overFullscreen) or a critical banner; banners and OSDs that no output can show are dropped.
  void setFullscreenOutputs(std::vector<wl_output*> outputs);
  // The lock screen editor is open: every face goes under it, so a banner never covers the editor's top bar.
  void setUnderEditor(bool underEditor);
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
  using MorphDriver = IslandMorphDriver<Face, Rect, AnimationManager::Id>;
  using Morph = MorphDriver::Morph;

  struct Instance {
    wl_output* output = nullptr;
    // Theme, palette and glass-slider changes re-apply the material, as on the bar and dock.
    Signal<>::ScopedConnection paletteConn;
    Signal<>::ScopedConnection glassConn;
    float width = 0.0F; // surface width: the widest card
    CenterIslandSpace space;
    float liveWidth = 0.0F; // widest card the free center space allows
    // Last blur and input regions sent; nullopt = none yet. A tick with the same rects sends nothing.
    std::optional<std::vector<InputRect>> sentBlur;
    std::optional<std::vector<InputRect>> sentInput;
    float restWidth = 0.0F;
    float restLength = 0.0F; // lane pill height, hugging its stacked content
    float restPadTop = 0.0F; // lane pill: rows above the stacked content
    float scale = 1.0F; // surface px per layout px
    std::unique_ptr<LayerSurface> surface;
    // Our compositor morphs the island (clip and radius) and draws its shadow; null on other compositors.
    std::unique_ptr<DskSurfaceClient> dsk;
    // In a bar lane on our compositor the pill and the card are two glass shapes, so the pill stays beside its card
    // and the surface clip spans the surface; null elsewhere. After `dsk`, so destroyed first.
    std::unique_ptr<DskShapeClient> pillShape;
    std::unique_ptr<DskShapeClient> cardShape;
    std::optional<DskSurfaceClient::Value> pillClip; // last clip sent to pillShape; nullopt = none yet
    bool cardOpen = false;                           // cardShape has a clip, so it draws glass
    // Rest-face waveform drawn by the compositor as flat looping shapes; after `dsk`, so destroyed first.
    std::array<std::unique_ptr<DskShapeClient>, 3> barShape;
    std::optional<std::array<float, 6>> barShapeKey; // x, y, r, g, b, a of the looping bars; empty = client bars
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
    Label* weekday = nullptr; // lane pill caption above the day number
    Glyph* callGlyph = nullptr;
    Box* recordDot = nullptr; // lane pill: the red dot of a compact recording
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
    Node* bannerActionRow = nullptr;
    std::array<Button*, 3> bannerActions{};
    Button* bannerClose = nullptr; // shown while the pointer is over the banner
    bool bannerHover = false;
    std::string artUrl;
    std::string hoverArtUrl;
    MorphDriver morph{Face::Rest};
    bool hovered = false;
    bool hoverMedia = false; // latched when the hover starts, so a pause does not swap the face under the pointer
    Timer collapse;
    float reveal = 1.0F; // 0..1 from `from` to `to`
    float osdProgress = 0.0F;
    float dragOffset = 0.0F; // banner swipe: surface translate along dragAxis, logical px (negative = up)
    BannerSwipeAxis dragAxis = BannerSwipeAxis::Undecided;
    float dragStartX = 0.0F; // pointer press, logical px; a scroll travels from 0
    float dragStartY = 0.0F;
    float dragTravelX = 0.0F; // scroll travel since the first scroll event
    float dragTravelY = 0.0F;
    float dragLastTravel = 0.0F; // along dragAxis, at dragLastAt
    float dragVelocity = 0.0F;   // px/ms along dragAxis
    Timer scrollEnd;             // the scroll drag releases when its events stop
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
    Timer driverReaper;
    double dskLevel = 0.0; // last elevation level sent (protocol version 4)
    bool compositorTint = false; // the compositor draws the glass tint; the fills paint nothing
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
    // (key, label) of the notification's actions besides "default", at most 3: §9.2 action buttons.
    std::vector<std::pair<std::string, std::string>> actions;
    // Critical or never expiring: no timeout; it stays until acted on or swiped away.
    bool sticky = false;
    bool critical = false; // shows over a fullscreen window
    bool respectsDnd = true;
  };

  void refreshNow();
  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, float width, float height);
  // The banner's §9.2 action buttons (hidden until a banner has actions); the first is the primary capsule.
  std::unique_ptr<Node> bannerActionRow(Instance& inst);
  void bannerAction(Instance& inst, std::size_t index);
  void applyContent(Instance& inst);
  void applyReveal(Instance& inst, float reveal);
  void animateReveal(Instance& inst, Face face, float velocity);
  void bannerDragStart(Instance& inst, float x, float y);
  void bannerDragMove(Instance& inst, float x, float y);
  void bannerDragTravel(Instance& inst, float dx, float dy);
  void bannerDragEnd(Instance& inst);
  [[nodiscard]] bool bannerScroll(Instance& inst, const InputArea::PointerData& d);
  void setBannerHover(Instance& inst, bool hover);
  void closeBanner();
  void startBannerSpring(Instance& inst, bool dismiss);
  [[nodiscard]] bool bannerDragStale(const Instance& inst) const;
  void finishBannerDrag(Instance& inst, bool dismiss);
  void applyBannerOffset(Instance& inst, float offset);
  // The signed dismissal end of dragAxis, logical px: up leaves one row on the output, right one column.
  [[nodiscard]] float bannerLimit(const Instance& inst) const;
  void updateFace(Instance& inst);
  void applyLayer(Instance& inst);
  void updateAllFaces();
  [[nodiscard]] Face desiredFace(const Instance& inst) const;
  [[nodiscard]] bool hidesBanner(const Instance& inst) const;
  // The activity hides the banner on every island, so the banner timer pauses.
  [[nodiscard]] bool bannerCovered() const;
  // True when the clip moved (animated or jumped); `continueMirror`: a mirrored morph was running before this change.
  bool syncDsk(Instance& inst, bool animate, bool continueMirror);
  // The glass tint of `face`: the pill material at rest, the sheet tint expanded; alpha 0 when the shell paints it.
  [[nodiscard]] static DskSurfaceClient::Value islandTint(const Instance& inst, Face face);
  void startMirror(Instance& inst, const Rect& target, bool grow, bool continueFrom);
  void stepMirror(Instance& inst);
  [[nodiscard]] AnimationManager::Id startMirrorDriver(Instance& inst);
  // A driver that ended on its own ends its morph at the target.
  void driverDone(Instance& inst, AnimationManager::Id driver);
  struct MorphHost;
  [[nodiscard]] static bool mirrored(const Instance& inst);
  // The morph just settled: the layer, elevation and swipe offset of the shown face.
  void finishReveal(Instance& inst);
  [[nodiscard]] static bool inLane(const Instance& inst);
  [[nodiscard]] static float surfaceWidth(const Instance& inst);
  [[nodiscard]] static float surfaceHeight(const Instance& inst);
  // The bars' box height: the art beside them in the row, the level group in the lane.
  [[nodiscard]] static float barsHeight(const Instance& inst);
  [[nodiscard]] static std::int32_t surfaceMargin(const CenterIslandSpace& space);
  [[nodiscard]] static std::uint32_t islandAnchor(const CenterIslandSpace& space);
  // Layer margins: top, right, bottom, left.
  [[nodiscard]] static std::array<std::int32_t, 4> surfaceMargins(const CenterIslandSpace& space);
  [[nodiscard]] static std::int32_t endMargin();
  [[nodiscard]] static float endPad(const Instance& inst);
  [[nodiscard]] Rect faceRect(const Instance& inst, Face face) const;
  // faceRect, but the outgoing face of a running morph keeps the card it had when the morph began.
  [[nodiscard]] Rect cardRect(const Instance& inst, Face face) const;
  void hoverEnter(Instance& inst);
  void hoverLeave(Instance& inst);
  // Closes the hover card now, without the collapse delay.
  void endHover(Instance& inst);
  void loadArt(Instance& inst, Renderer& renderer, const std::string& artUrl);
  void syncBars();
  void tickBars();
  void syncBarShapes(Instance& inst);
  [[nodiscard]] static bool laneGlass(const Instance& inst);
  // The activity rests as a compact face (LiveActivity::since and compact) on this island.
  [[nodiscard]] bool compactActivity(const Instance& inst) const;
  // Runs the 1 s elapsed-time refresh while any island shows a compact activity; on every activity or island change.
  void syncElapsedTimer();
  // The lane pill as a sheet's source, so the sheet sits beside the bar at the island's top; nullopt in a bar row.
  [[nodiscard]] std::optional<SheetSource> laneSheetSource(const Instance& inst) const;
  // The lane pill's glass shape: at the pill while `shown`, else no clip.
  void syncLanePill(Instance& inst, bool shown) const;
  // The card shape follows `face`: grown from the pill to its card, or shrunk back into the pill and then cleared.
  void syncLaneCard(Instance& inst, Face face, bool jump, bool continueMirror);
  [[nodiscard]] bool restUnderFullscreen(const Instance& inst) const;
  [[nodiscard]] bool fullscreenOn(const Instance& inst) const;
  [[nodiscard]] bool everyIslandFullscreen() const;
  // The settled rest pill faded out with the bar's auto-hide: nothing draws, loops or takes input.
  [[nodiscard]] static bool restHidden(const Instance& inst);
  [[nodiscard]] bool realSpectrum() const;
  [[nodiscard]] bool barsNeedTimer() const;
  void syncHoverProgress(Instance& inst);
  using Queue = IslandQueue<BannerItem, OsdContent>;
  void apply(Queue::Step step);
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
  OpenCallback m_open;
  std::function<bool()> m_hoverGate;
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
  Timer m_bannerTimer;
  // The banner whose timeout a hover paused; it restarts in full when the pointer leaves.
  std::optional<std::uint32_t> m_bannerPausedFor;
  Queue m_queue;
  // Read-only views: only m_queue changes the OSD and the banners.
  const std::optional<OsdContent>& m_osd = m_queue.osd();
  const std::optional<BannerItem>& m_banner = m_queue.banner();
  Timer m_callTimer;
  IconResolver m_iconResolver;
  int m_notificationToken = -1;
  // Ids the island showed as a banner: their later updates never raise a toast.
  std::unordered_set<std::uint32_t> m_consumed;
  std::optional<LiveActivity> m_activity;
  // Kept while the island shrinks after the activity ended.
  LiveActivity m_shown;
  std::unordered_set<wl_output*> m_visibleOutputs;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  bool m_refreshScheduled = false;
  std::vector<std::unique_ptr<Instance>> m_instances;
  std::vector<wl_output*> m_fullscreenOutputs;
  bool m_underEditor = false;
};
