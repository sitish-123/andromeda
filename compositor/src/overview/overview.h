#pragma once

#include "config/config.h"
#include "core/animation.h"
#include "layout/drop_target.h"
#include "overview/app_icons.h"
#include "overview/card_pointer.h"
#include "overview/chrome.h"
#include "overview/navigation.h"
#include "overview/style.h"
#include "scene/hint_rect.h"
#include "scene/surface_blur.h"
#include "util/listener.h"
#include "util/tracked.h"
#include "view/chrome_attachment.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_scene_buffer;
struct wlr_scene_border;
struct wlr_scene_blur;
struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_pointer;

namespace andromeda {

  enum class KeybindAction;
  class LayerSurface;
  class Output;
  class Server;
  class View;
  class Workspace;
  class WorkspaceGroup;

  // Zoomed-out view of every workspace on every output, arranged by `[overview] style` (overview/style.h). Clients
  // are never reconfigured: the real window trees are hidden and re-rendered as "cards", per-surface scene buffers
  // sharing the client textures and scaled by the scene graph. Each card moves from its desktop rect to its placement
  // rect with the open progress. The overview is also an editor (click to focus, middle-click to
  // close, drag to relocate), so it owns pointer and keyboard input while open.
  class Overview : public Animatable {
  public:
    explicit Overview(Server& server);
    ~Overview();

    Overview(const Overview&) = delete;
    Overview& operator=(const Overview&) = delete;

    // Open, opening, or zooming back in.
    [[nodiscard]] bool active() const { return m_active; }
    // 0 closed .. 1 open, as drawn this frame (gesture-driven or animating).
    [[nodiscard]] double progress() const { return m_progress; }
    // Open and not already zooming back in: pointer/keyboard edits still apply. The IPC `overview` event reports this
    // as `open`, so clients drop overview-scoped surfaces while the windows are still hidden.
    [[nodiscard]] bool interactive() const { return m_active && !m_closing; }
    // The finger's progress (0 closed, 1 open) while a swipe drives the overview; the IPC `overview` event sends it so
    // shell chrome follows the hand instead of its own spring.
    [[nodiscard]] std::optional<double> fingerProgress() const { return m_fingerProgress; }
    // True while a card mirrors `view`, so its client must keep drawing although its real frame is hidden.
    [[nodiscard]] bool presentsLive(const View* view) const;

    void toggle();
    void open();
    // Zoom back into each output's active workspace, restoring normal focus.
    void close();
    // Activate `workspace` (no slide, the real trees are hidden), focus `focus`
    // so its column reveal shares the closing zoom, then restore keyboard focus
    // once the zoom lands. Null lets normal refocus choose the target.
    void closeToWorkspace(Workspace* workspace, View* focus);
    // Instant teardown with no animation (session lock, config reload, output loss).
    void forceClose();

    // 4-finger swipe. `progress` is rubber-banded by the caller, so it can sit slightly outside [0, 1] at either end.
    // `releaseVelocity` is the speed the fingers were still moving at when they left, in overview progress per second,
    // and is zero for callers that are not a gesture.
    void gestureUpdate(double progress);
    // `stop`: 0 closed, 1 open.
    void gestureEnd(int stop, double releaseVelocity = 0);
    // Stops a running open or close where it is drawn and returns that progress, for a swipe to start from.
    // `heading` (may be null) receives the state (0 closed, 1 open) the stopped motion was going to; `caught` (may be
    // null) whether a running open or close was stopped.
    [[nodiscard]] double gestureGrab(int* heading = nullptr, bool* caught = nullptr);
    [[nodiscard]] bool zoomAnimating() const { return m_zoomAnim.animating(); }

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    // Advances the zoom and workspace-row animations; returns true while either is still running.
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    // The overview zooms every output at once.
    [[nodiscard]] bool animatesOn(const Output* /*output*/) const override { return true; }

    void onViewMapped(View* view);
    void onViewUnmapped(View* view, bool animateClose = true);
    void onViewPinnedChanged(View* view);
    void onViewFloatingChanged(View* view);
    void onViewWorkspaceChanged(View* view);
    // A dialog attached to or left `parent`: its card mirrors the dialogs over the window.
    void onViewAttachmentsChanged(View* parent);
    void onWorkspaceActivated(WorkspaceGroup* group);
    void onWorkspaceArranged(Workspace* workspace);
    void onWorkspaceInventoryChanged(WorkspaceGroup* group);
    void onFocusChanged();
    // Rebind cached selections after renderer replacement without advancing policy.
    void refreshEffectBindings();
    // Coalesce card geometry refreshes from view resize animation ticks. Views
    // advance before overlays, so the overview consumes the latest presented
    // size once per frame regardless of how many views resized together.
    void onViewPresentationChanged(View* view);
    void onOutputRemoved(Output* output);
    // A background- or bottom-layer surface on `output` mapped, unmapped, or changed layer: the mirrored stack has
    // to catch up while the real bottom layer is hidden.
    void onDesktopLayerChanged(Output* output);

    // Input entry points; called from Cursor/Keyboard while active.
    bool handleButton(uint32_t button, bool pressed, double lx, double ly, uint32_t timeMsec);
    void handleMotion(double lx, double ly, uint32_t timeMsec);
    bool handleAxisNotch(bool vertical, double direction, double lx, double ly);
    // Swipe and finger-scroll input both arrive as content-direction deltas and share one navigation lifetime; the
    // source only selects the travel distances and decides who may end the gesture. Scroll samples are combined at
    // the pointer frame.
    void beginNavigation(wlr_pointer* pointer, NavigationSource source, double lx, double ly);
    void updateNavigation(double dx, double dy, uint32_t timeMsec);
    void endNavigation(bool cancelled, uint32_t timeMsec, NavigationSource source);
    // Drop an unfinished gesture whatever its source, leaving the filmstrip to settle on the active workspace.
    void cancelNavigation();
    void handleTouchpadAxis(wlr_pointer* pointer, bool vertical, double delta, uint32_t timeMsec, double lx, double ly);
    void handleTouchpadFrame();
    // `shift`: Shift is held (Shift+arrows move the selected window to the neighbouring workspace). `ctrl`: Ctrl is held
    // (Ctrl+W closes the selected window); no other Ctrl key reaches the overview.
    bool handleFallbackKey(uint32_t keysym, bool shift = false, bool ctrl = false);
    // Configured actions retain their regular handlers throughout the closing animation.
    bool handleKeybindAction(KeybindAction action);
    // Step the active workspace `delta` rows along the workspaces on `output` (null: wherever the pointer is). Returns
    // false at either end. The wheel and middle-button drag use discrete steps;
    // touchpad navigation moves the rows continuously and selects on release.
    bool selectRelativeWorkspace(int delta, Output* output);
    // The workspace row a pointer drag at this point pans. The row extends along its scrolling axis across the
    // whole output, because its cards may overhang the centered workspace preview.
    [[nodiscard]] Workspace* pointerScrollWorkspace(double lx, double ly);
    // How much smaller than on the desktop `workspace` shows in the overview, so a pointer pan over it moves its
    // content under the pointer. 1 when it is not shown.
    [[nodiscard]] double workspaceScale(const Workspace* workspace) const;
    [[nodiscard]] bool dragging() const { return dragCard() != nullptr || m_middlePressed; }

  private:
    static void onNavigationDeviceDestroyed(Overview* self, void* data);
    [[nodiscard]] Workspace* navigationWorkspace() const;
    OverviewNavigation m_navigation;
    Output* m_navigationOutput = nullptr;
    Ref<Workspace> m_navigationWorkspace;
    wlr_pointer* m_navigationPointer = nullptr;
    Listener m_navigationDeviceDestroy;
    NavigationSource m_navigationSource = NavigationSource::Scroll;
    bool m_navigationHorizontalWorkspaces = false;
    bool m_navigationStarted = false;
    double m_navigationStart = 0;
    double m_navigationScale = 1;
    bool m_navigationCentered = false;
    // Finger-scroll deltas accumulated since the last pointer frame, and whether that axis reported a stop.
    double m_scrollDx = 0;
    double m_scrollDy = 0;
    bool m_scrollStopX = false;
    bool m_scrollStopY = false;
    uint32_t m_scrollTime = 0;
    struct Card;
    struct OutputState;

    struct CardSurface {
      Card* card = nullptr;
      // The window whose surface tree holds `surface`: the card's own, or a dialog attached to it.
      View* view = nullptr;
      wlr_surface* surface = nullptr;
      wlr_scene_buffer* sourceBuffer = nullptr;
      wlr_scene_buffer* buffer = nullptr;
      int sx = 0;
      int sy = 0;
      bool isRoot = false;
      // Whether the latest walk of the card's surfaces reached `surface`. A surface that left the tree keeps its entry
      // until destroyed, and `sourceBuffer` may already be freed with its subsurface role.
      bool inTree = true;
      Listener commit;
      Listener destroy;
      Listener outputSample;
      Listener frameDone;
    };

    struct Card : Trackable {
      Overview* overview = nullptr;
      OutputState* owner = nullptr;
      View* view = nullptr;
      size_t workspaceIndex = 0; // workspace index inside the output's group
      wlr_scene_tree* tree = nullptr;
      // Parent of every `surfaces` buffer, at the card origin: the card's window and overlay slots live here, as they
      // live on the view's surface tree.
      wlr_scene_tree* surfaceTree = nullptr;
      wlr_scene_border* border = nullptr;
      int borderPadding = 0; // effect padding the ring was last laid out with, scaled
      SurfaceBlur blur;
      // Scaled copy of the view's drop shadow. Its tree lives under `tree` or, for a view whose shadow the workspace
      // pools below every tile, under the output's `tileShadows`.
      wlr_scene_tree* shadowTree = nullptr;
      wlr_scene_shadow* shadow = nullptr;
      // Scaled copy of the chrome the view carries beside its borders, such as a tab bar, under `tree`.
      std::unique_ptr<ViewChromeAttachment> chrome;
      std::unique_ptr<ViewChromeAttachment> frame;
      std::vector<std::unique_ptr<CardSurface>> surfaces;
      wlr_box box{}; // content box in layout coordinates
      // Card scale against the view's presented box, its placement opacity and its clip in card-local pixels (empty:
      // none), as the latest output layout left them.
      double scale = 1.0;
      float opacity = 1.0F;
      wlr_box clip{};
    };

    // One surface of the output's mirrored stack: a background- or bottom-layer surface copied into every workspace
    // preview. `tree` is the layer surface's own scene tree, re-resolved every layout, and the source of both the
    // mirror geometry and its color state.
    struct DesktopSurface {
      Overview* overview = nullptr;
      OutputState* state = nullptr;
      wlr_surface* surface = nullptr;
      wlr_scene_tree* tree = nullptr;
      Listener commit;
      Listener destroy;
    };

    // One mirrored surface inside one row. Every copy paces its client: the real bottom layer is hidden while the
    // overview is open, so these buffers are the only place its surfaces are sampled.
    struct DesktopMirror {
      DesktopSurface* source = nullptr;
      wlr_scene_buffer* buffer = nullptr;
      Listener outputSample;
      Listener frameDone;
    };

    // Resolved stack entry, ordered bottom to top.
    struct DesktopEntry {
      wlr_surface* surface = nullptr;
      wlr_scene_tree* tree = nullptr;
    };

    // One workspace row's backdrop: the flat fill, plus one mirror per stack surface drawn over it when
    // `[overview] workspace_wallpaper` is on. The fill is what an output with an empty stack shows.
    struct WorkspaceBackground {
      wlr_scene_tree* tree = nullptr;
      wlr_scene_rect* fill = nullptr;
      std::vector<std::unique_ptr<DesktopMirror>> mirrors;
    };

    // Where one workspace shows at the current progress: its card in layout coordinates and the map from its
    // output-local desktop into the overview. The chosen workspace of Spaces and Task View maps into the main area.
    struct WorkspaceView {
      wlr_box box{};
      ov::Fit fit{};
      ov::CardKind kind = ov::CardKind::Frame;
      bool shown = false;
    };

    struct OutputState {
      Output* output = nullptr;
      // Refreshed by every layoutOutput. `usableBox` is the output minus the layer-shell exclusive zones: panels of
      // the top and overlay layers draw over the overview, so on-output is not on-screen for anything readable.
      wlr_box outputBox{};
      wlr_box usableBox{};
      std::vector<WorkspaceView> workspaceViews;
      // Spaces and Task View: the chosen workspace's windows fill this area instead of its card.
      std::optional<wlr_box> mainBox;
      ov::Fit mainFit{};
      size_t mainWorkspace = 0;
      // Cards step along x (thumb rows, grid rows, a horizontal strip) rather than y.
      bool cardsHorizontal = false;
      // The settled placement, recomputed only when its input, the chosen row or the style changes, so an open or
      // close in flight only interpolates. `placeOf[k]` is card k's entry, or null.
      ov::Input placedInput;
      ov::Placement placement;
      std::vector<const ov::WindowPlace*> placeOf;
      ov::Style placedStyle = ov::Style::Spaces;
      double placedRow = 0;
      bool placed = false;
      // What the placement is made of: the style, each workspace's arrangement and its windows. Only a change here (a
      // window opening, closing or minimizing, a workspace added or removed, a style reload) reflows the overview;
      // rows scrolling and windows moving under their own layout springs do not.
      std::vector<uintptr_t> structure;
      // Per-tick scratch, kept for its capacity.
      std::vector<uintptr_t> nextStructure;
      std::vector<DesktopEntry> resolvedDesktop;
      wlr_scene_tree* tree = nullptr;
      wlr_scene_blur* backgroundBlur = nullptr;
      wlr_scene_rect* backgroundTint = nullptr;
      // Card shadows the real scene draws below every tile, above the workspace backgrounds and below every card.
      wlr_scene_tree* tileShadows = nullptr;
      std::vector<WorkspaceBackground> workspaceBackgrounds;
      std::vector<std::unique_ptr<Card>> cards;
      std::vector<std::unique_ptr<DesktopSurface>> desktop;
      // Names, title row, filter field, hint, + target and title capsules, above every card.
      std::unique_ptr<ov::Chrome> chrome;
      // The chosen workspace, fractional while it moves: 1.5 sits halfway between workspaces 1 and 2. Strip scrolls
      // its rows by it, Spaces and Task View morph between neighbours. Wheel steps, keyboard moves and touchpad
      // releases all animate this one value; a gesture in flight snaps it to follow the fingers.
      AnimatedValue rowScroll;
      size_t activeWorkspaceIndex = 0;
    };

    static void onCardSurfaceCommit(CardSurface* entry, void* data);
    static void onCardSurfaceDestroy(CardSurface* entry, void* data);
    static void onCardBufferOutputSample(CardSurface* entry, void* data);
    static void onCardBufferFrameDone(CardSurface* entry, void* data);
    // One walk over a card's surfaces, for one window of it, offset from the card window's root surface.
    struct CardWalk {
      Card* card = nullptr;
      View* view = nullptr;
      int dx = 0;
      int dy = 0;
    };
    // The card window's surfaces, then those of every dialog attached to it at their offset over the window.
    static void forEachCardSurface(Card* card, void (*visit)(wlr_surface*, int, int, void*));
    static void addCardSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void syncCardSurface(wlr_surface* surface, int sx, int sy, void* data);
    static void syncCardSurfaces(Card* card);
    static void onDesktopSurfaceCommit(DesktopSurface* source, void* data);
    static void onDesktopSurfaceDestroy(DesktopSurface* source, void* data);
    static void onDesktopMirrorOutputSample(DesktopMirror* mirror, void* data);
    static void onDesktopMirrorFrameDone(DesktopMirror* mirror, void* data);

    // Placement input for one output: every laid-out card as a member of its workspace, id = index in `state.cards`.
    // `targets` reads where the layout is heading instead of where windows show now.
    [[nodiscard]] ov::Input placementInput(const OutputState& state, bool targets) const;
    // How much of the desktop's own chrome (window shadows, pinned windows) shows: 1 at rest on the desktop, fading to
    // 0 over the first stretch of the open, so a close brings it back while the zoom is still settling.
    [[nodiscard]] float desktopChromeAlpha() const;
    // Upper bound on how far any visible card pixel moves per unit of the open or row animation value, for ending a
    // spring once its remaining motion can no longer change a pixel.
    [[nodiscard]] double motionPixelsPerUnit(bool rows) const;
    // Re-reads the output and usable boxes; false while the output has no size.
    bool refreshOutputBoxes(OutputState& state) const;

    bool beginPresentation();
    void buildState();
    void populateCards(OutputState& state);
    Card* createCard(OutputState& state, View* view, size_t workspaceIndex);
    void snapshotCardForClose(Card& card);
    void destroyCard(Card* card);
    void layoutCardShadow(Card& card, float alpha) const;
    static void destroyCardShadow(Card& card);
    // Lay the card's chrome copy over `world`, the view's presented box, at `zoom`, or drop it with the view's chrome.
    void layoutCardChrome(Card& card, const wlr_box& world, int borderInset, double zoom, float alpha) const;
    static void syncCardBuffer(CardSurface& entry);
    void dropCard(View* view);
    void rebuildCard(View* view);
    [[nodiscard]] OutputState* stateFor(const Output* output);
    [[nodiscard]] OutputState* stateForWorkspace(const Workspace* workspace);
    [[nodiscard]] Card* findCard(const View* view);
    void updateHover(double lx, double ly);
    [[nodiscard]] const View* closeButtonAt(double lx, double ly) const;
    // The arrangement menu, open under a workspace's chip on one output. Compared, never dereferenced.
    struct ArrangementMenu {
      const Output* output = nullptr;
      size_t workspace = 0;
      int hovered = -1;
    };
    // Opens, steers and applies the arrangement menu; each returns false when it did not take the input.
    bool menuPress(double lx, double ly);
    bool menuKey(uint32_t keysym);
    void setMenu(std::optional<ArrangementMenu> menu);
    // Right-click window menu: Move to each other workspace, Move to New Desktop, Close, Close All on its workspace.
    struct WindowMenu {
      const Output* output = nullptr;
      Ref<View> view;
      int x = 0;
      int y = 0;
      int hovered = -1;
      enum class Action { MoveToWorkspace, MoveToOutput, Rename, Arrangement, Close, CloseAll };
      struct Row {
        Action action = Action::Close;
        Ref<Workspace> workspace; // MoveToWorkspace target; Rename, Arrangement: the card's workspace
        Ref<Output> output;       // MoveToOutput: the display, whose active workspace is resolved on apply
        bool newWorkspace = false; // MoveToWorkspace into a new workspace
      };
      std::vector<std::string> rows;
      std::vector<Row> actions; // one per row
    };
    void openWindowMenu(const Card& card, double lx, double ly);
    void setWindowMenu(std::optional<WindowMenu> menu);
    void applyWindowMenuRow(int row);
    // Moves `view` to `target` and keeps the overview open; a null target is a new workspace at the end.
    void moveViewTo(View& view, Workspace* target);
    // Steers the open window menu; false when none is open.
    bool windowMenuPress(double lx, double ly);
    bool windowMenuKey(uint32_t keysym);
    void applyMenuRow(int row);
    // Renames `workspace`, or the preferred output's active workspace when null.
    void startRename(Workspace* workspace = nullptr);
    bool renameKey(uint32_t keysym);
    // Keeps the typed name (a blank one keeps the old name) or drops it, and ends the rename.
    void finishRename(bool keep);
    [[nodiscard]] WorkspaceBackground createWorkspaceBackground(OutputState& state) const;
    // Mapped background- and bottom-layer surfaces of `output`, in render order.
    void collectDesktopSurfaces(const Output& output, std::vector<DesktopEntry>& out) const;
    // Re-resolves the output's mirrored stack, rebuilding every row's mirrors when its surfaces changed.
    void refreshDesktop(OutputState& state);
    void clearDesktop(OutputState& state) const;
    void createRowMirrors(OutputState& state, WorkspaceBackground& background) const;
    // Points every mirror at its surface's committed buffer and copies its color state.
    void syncDesktopMirrors(const OutputState& state) const;
    [[nodiscard]] static bool desktopSourceBox(const DesktopSurface& source, wlr_box& out);

    void applyProgress();
    // Applies `alpha` to every pinned window and full opacity to every other.
    void applyPinnedOpacity(float alpha) const;
    void layoutOutput(OutputState& state);
    void layoutChrome(OutputState& state, const View* liveTarget);
    // Lays the card out at its stored box, scale, opacity and clip.
    void layoutCard(Card& card, const View* liveTarget);
    // The workspace whose card or main area holds the point, on its output only.
    [[nodiscard]] static bool workspaceViewContains(const OutputState& state, size_t index, double lx, double ly);
    // Refreshes a card's animation and persistent effect slots from its view.
    void syncCardEffects(Card& card);
    void syncCardEffects();
    // The window a focus or close action would act on right now: the focused view of the active workspace on the
    // output holding the cursor. Null when that workspace is empty, which is also when those actions do nothing.
    [[nodiscard]] View* liveTargetView() const;
    [[nodiscard]] std::array<float, 4> cardBorderColor(const Card& card, const View* liveTarget) const;
    // Type-to-filter: `m_filter` dims the cards whose title and app id miss it and moves the selection onto a match.
    [[nodiscard]] bool cardMatches(const Card& card) const;
    // Live window cards in reading order, the active workspace on the pointer's output first; matches only.
    [[nodiscard]] std::vector<Card*> selectableCards();
    [[nodiscard]] Card* cardOf(const View* view);
    // The window Enter, Delete and Tab act on: the live target while it matches the filter.
    [[nodiscard]] Card* selectedCard();
    void selectCard(Card& card);
    void setFilter(std::string filter);
    bool moveSelection(int dx, int dy);
    bool cycleSelection(int step);
    bool activateFilter();

    // `releaseVelocity` is the speed a gesture left behind, in overview progress per second, and is zero for every
    // other caller: it starts the zoom spring from the speed the fingers had instead of from rest.
    void startAnimation(double target, bool closing, double releaseVelocity = 0);
    // Move one output's filmstrip onto row `target`. `releaseVelocity` is the speed a touchpad gesture left behind,
    // in rows per second, and is zero for every other caller.
    void animateRow(OutputState& state, double target, double releaseVelocity = 0);
    void finishAnimation();
    void beginClose(View* focus, double releaseVelocity = 0);
    void teardown();
    // Hand the seat back after teardown: `focus`, or normal refocus when null or unmapped.
    void restoreFocus(View* focus);
    void scheduleFrames() const;

    [[nodiscard]] Card* cardAt(double lx, double ly, const Card* skip = nullptr);
    [[nodiscard]] Workspace*
    workspaceAtPoint(double lx, double ly, OutputState** outState, size_t* outIndex, bool extendScrollingAxis);
    [[nodiscard]] WorkspaceGroup*
    workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox);
    [[nodiscard]] Workspace* preferredWorkspace() const;
    void clearMiddlePress();

    void beginDrag();
    void updateDrag(double lx, double ly);
    // Recomputes the drop target under the pointer without moving the dragged card: rows can slide under a still one.
    void retargetDrop(double lx, double ly);
    // Edge auto-scroll: holding a dragged card within kEdgeZone of an output edge steps the rows along the workspace
    // axis (Strip) or pans a scrolling workspace across it, after kEdgeDelayMs and then every kEdgeRepeatMs.
    void updateEdgeScroll(double lx, double ly);
    void stopEdgeScroll();
    // Moves an armed pan by the frame's elapsed time; true while it can still move.
    bool tickEdgePan(double nowMsec);
    static int onEdgeTimer(void* data);
    // The new-workspace target under the pointer, as an insertion at the end of its output's workspaces.
    [[nodiscard]] WorkspaceGroup* plusTargetAt(double lx, double ly, OutputState** outState, wlr_box* outBox);
    void endDrag(bool drop);
    // Inserts `view` back into the slot beginDrag detached the dragged window from.
    void restoreDragSlot(View& view);
    // The tiled window whose middle is under the pointer, which a drop swaps with the dragged one.
    [[nodiscard]] View* swapTargetAt(double lx, double ly);
    void swapDragged(View& view, View& other);
    [[nodiscard]] bool cardInThumb(const Card& card) const;
    // The workspace whose thumb or Grid tile is under the point (not a Frame or the main area), and its box.
    [[nodiscard]] Workspace* thumbAt(double lx, double ly, wlr_box* outBox = nullptr);
    // A thumb the pressed one can be reordered onto: another workspace of the same output.
    [[nodiscard]] Workspace* thumbDropAt(double lx, double ly, wlr_box* outBox);
    void updateThumbDrag(double lx, double ly);
    void endThumbDrag(bool drop, double lx, double ly);
    void syncWorkspaceRows(OutputState& state, WorkspaceGroup& group);
    // The desk->overview map of workspace `index` on `state`: the main area for the chosen workspace when it has one.
    [[nodiscard]] static ov::Fit workspaceFit(const OutputState& state, size_t index);
    void showDropHint(const wlr_box& worldBox, const OutputState& state, size_t workspaceIndex);
    void showWorkspaceInsertHint(Output* output, const wlr_box& box);
    void hideDropHint();

    Server* m_server = nullptr;
    wlr_scene_tree* m_tree = nullptr; // Server::overviewTree()
    std::unique_ptr<HintRect> m_dropHint;
    std::unique_ptr<HintRect> m_edgeHint;
    wl_event_source* m_edgeTimer = nullptr;
    // The edge a drag is held at: the output it is on, and +-1 along (step rows) or across (pan) the workspace axis.
    Output* m_edgeOutput = nullptr;
    int m_edgeStep = 0;
    int m_edgePan = 0;
    Ref<Workspace> m_edgeWorkspace; // the scrolling workspace an armed pan moves
    double m_edgePanLastMsec = -1.0; // frame time of the last pan step; < 0 before the first frame
    double m_dragX = 0;
    double m_dragY = 0;
    std::vector<std::unique_ptr<OutputState>> m_outputs;

    bool m_active = false;
    bool m_closing = false;
    double m_progress = 0;
    double m_targetProgress = 0;
    double m_progressFrom = 0;
    AnimatedValue m_zoomAnim;
    // Reflow: one shared spring from 0 to 1 carries every card and workspace card whose placed rect jumped in a
    // structural change from where it was drawn to its new place. Keys are View and Workspace pointers, rects are
    // layout coordinates before the open progress, so a card rebuilt or dropped on another output still glides.
    // Scratch lists layoutChrome refills every frame, kept to avoid reallocating them.
    std::vector<ov::ChromeWorkspace> m_chromeWorkspaces;
    std::vector<std::string> m_chromeNames; // owns the names m_chromeWorkspaces views
    std::vector<ov::ChromeWindow> m_chromeWindows;
    std::unique_ptr<ov::AppIcons> m_appIcons;
    AnimatedValue m_reflow{1.0};
    std::unordered_map<const void*, ov::Rect> m_shownRects;
    std::unordered_map<const void*, ov::Rect> m_reflowFrom;
    void beginReflow();
    ov::Rect reflowRect(const void* key, const ov::Rect& target, bool began);
    Ref<View> m_pendingFocus;
    bool m_cardPresentationDirty = false;
    bool m_gestureOpenedHere = false;
    std::optional<double> m_fingerProgress;
    // Logs every frame of a swipe and its release at debug level.
    bool m_traceFrames = false;
    // Frame clock of the last tick, in ms.
    double m_lastTickMsec = 0.0;
    double m_fingerTarget = 0.0;
    std::optional<std::chrono::steady_clock::time_point> m_gestureOpenedAt;
    std::string m_filter;
    // Output under the pointer, which is the output the live target resolves against.
    Ref<Output> m_pointerOutput;

    ov::CardPointer<Card> m_pointer;
    [[nodiscard]] Card* pressCard() const { return m_pointer.pressed(); }
    [[nodiscard]] Card* dragCard() const { return m_pointer.dragged(); }
    Ref<Workspace> m_pressWorkspace;
    bool m_pressThumb = false;    // the press landed on a thumb, so travel reorders workspaces
    bool m_thumbDragging = false; // that press has moved past kDragThreshold
    // The window card under the pointer and whether the pointer is on its close button. Compared, never dereferenced.
    const View* m_hoverView = nullptr;
    bool m_closeHover = false;
    // The window whose close button a left press landed on; a release on the same button closes it.
    const View* m_closePress = nullptr;
    std::optional<ArrangementMenu> m_menu;
    std::optional<WindowMenu> m_windowMenu;
    // F2 rename of the title row's workspace: the text so far; `fresh` until the first key, which replaces the name.
    struct Rename {
      Ref<Workspace> workspace;
      std::string text;
      bool fresh = true;
    };
    std::optional<Rename> m_rename;
    uint32_t m_titlePressMsec = 0; // the last press on the title name, for a double-click
    double m_pressX = 0;
    double m_pressY = 0;
    Ref<Card> m_middlePressCard;
    Ref<Output> m_middleOutput;
    double m_middlePressX = 0;
    double m_middlePressY = 0;
    // Travel along the pressed output's workspace axis since the last step.
    double m_middleAccum = 0;
    bool m_middlePressed = false;
    bool m_middleDragging = false;
    // Set once a drag locks across the workspace axis, whether or not a strip was there to pan.
    bool m_middlePanning = false;
    bool m_middleScrolling = false;

    double m_dragOffsetX = 0;
    double m_dragOffsetY = 0;
    Ref<Workspace> m_dragSourceWorkspace;
    int m_dragSourceColumn = -1;
    int m_dragSourceRow = -1;
    std::optional<DropColumnWidth> m_dragSourceWidth;
    bool m_dragSourcePromotes = false; // lifting the dragged window promoted a stack window into the master area
    DropTarget m_drop{};
    WorkspaceGroup* m_dropWorkspaceGroup = nullptr;
    size_t m_dropWorkspaceIndex = 0;
  };

} // namespace andromeda
