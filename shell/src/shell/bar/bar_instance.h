#pragma once

#include "shell/bar/center_island_space.h"

#include "config/config_service.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/bar/widget.h"
#include "shell/panel/attached_panel_context.h"
#include "ui/signal.h"
#include "wayland/layer_surface.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class Box;
class DskSurfaceClient;
class DskTrackerClient;
class Flex;
class Node;

struct BarCapsuleRun {
  Node* shell = nullptr;
  Box* bg = nullptr;
  Flex* container = nullptr;
  Node* content = nullptr;
  WidgetBarCapsuleSpec spec{};
  float contentScale = 1.0F;
  // Capsule geometry can exist without a painted fill or border.
  bool hasPaintedCapsuleBackground = false;
  std::vector<Widget*> widgets;
  // Hover highlight overlays, parallel to `widgets` for group runs; one shared box for single runs.
  std::vector<Box*> hoverBoxes;
  bool accordion = false;
  // Hover time in milliseconds before unfolding starts (expand only; collapse is immediate).
  float accordionDelayMs = 0.0F;
  // Pending delayed expand; cancelled when the pointer leaves, the state resolves, or the run dies.
  Timer accordionDelayTimer;
  // Clips accordion members to the reveal window (inside the capsule padding).
  Node* accordionClip = nullptr;
  BarAccordionDirection accordionDirection = BarAccordionDirection::End;
  // Index into `widgets` (visual order) of the always-visible member (config members[0]).
  std::size_t accordionVisibleIndex = 0;
  bool accordionExpanded = false;
  float accordionProgress = 0.0F; // 0 = collapsed, 1 = fully expanded
};

struct BarInstance {
  std::uint32_t outputName = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t outputLogicalX = 0;
  std::int32_t outputLogicalY = 0;
  std::int32_t outputLogicalWidth = 0;
  std::int32_t outputLogicalHeight = 0;
  std::size_t barIndex = 0;
  BarConfig barConfig;
  std::unique_ptr<LayerSurface> surface;
  // Drag on the end island opens the control center under the finger (top islands bar with dsk_surface_v1). Declared
  // after `surface`: it is destroyed first. Shared so an opening panel can hold it weakly.
  std::shared_ptr<DskTrackerClient> sheetTracker;
  std::array<float, 4> sheetTrackerRect{};
  // Liquid glass on the islands (dsk_surface_v1 v5); destroyed before `surface`.
  std::unique_ptr<DskSurfaceClient> dsk;
  // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  Node* slideRoot = nullptr;
  float slideHiddenDx = 0.0F;
  float slideHiddenDy = 0.0F;
  InputDispatcher inputDispatcher;
  // Gestures for the parts of the bar no widget covers. The sink is never mounted in the scene; it
  // is used only for its scroll-detent accumulator, so dead-zone scrolling quantizes like a widget.
  noctalia::bar::WidgetActionBindings deadZoneBindings;
  InputArea deadZoneAxisSink;
  float hideOpacity = 1.0F;
  AnimationManager::Id slideAnimId = 0;
  std::vector<InputRect> islandBlurStrips; // last island region sent; empty = none sent
  // bar-hide/toggle IPC on non-autohide bars: release compositor exclusive zone until bar-show (v4 isVisible=false).
  bool ipcLayoutReleased = false;
  // bar-auto-hide-set off keeps autoHide true until the reveal completes; block hover helpers from replacing it.
  bool autoHideDisablePending = false;
  Timer autoHideDelayTimer;
  // smart_auto_hide: active workspace empty (or overview open) — keep the bar visible.
  bool smartAutoHidePinnedVisible = false;
  bool pointerInside = false;
  float lastPointerSx = 0.0F;
  float lastPointerSy = 0.0F;
  std::size_t attachedPopupCount = 0;
  // Last region sent for dead_zone.click_through; skips resending an unchanged region every layout pass.
  std::optional<std::vector<InputRect>> clickThroughInputRegion;

  // Bar background, shadow, and layout sections (start/center/end along main axis)
  Box* bg = nullptr;
  // Islands mode: one pill per section (start, center, end), in slideRoot coordinates.
  std::array<Box*, 3> islands{};
  // Glass recipe of each island: drop shadow below it and the top edge highlight above its fill.
  std::array<Box*, 3> islandShadows{};
  std::optional<RoundedRectStyle> islandShadowStyle;
  ShadowDirectionOffset islandShadowOffset{};
  std::array<float, 6> lastCenterSpaceKey{-2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  std::optional<CenterIslandSpace> centerSpace;
  Box* shadow = nullptr;
  Node* shadowLeftClip = nullptr;
  Node* shadowRightClip = nullptr;
  Box* shadowLeft = nullptr;
  Box* shadowRight = nullptr;
  Node* contentClip = nullptr;
  // Unclipped layer between the bar background and contentClip; hosts the hover pills of
  // capsule-less widgets so they neither affect layout nor get clipped at section boundaries.
  Node* hoverUnderlay = nullptr;
  Node* startSlot = nullptr;
  Node* centerSlot = nullptr;
  Node* endSlot = nullptr;
  Flex* startSection = nullptr;
  Flex* centerSection = nullptr;
  Flex* endSection = nullptr;

  std::vector<std::unique_ptr<Widget>> startWidgets;
  std::vector<std::unique_ptr<Widget>> centerWidgets;
  std::vector<std::unique_ptr<Widget>> endWidgets;
  std::vector<BarCapsuleRun> startCapsuleRuns;
  std::vector<BarCapsuleRun> centerCapsuleRuns;
  std::vector<BarCapsuleRun> endCapsuleRuns;

  // Maps each widget's root node to its Widget so hover-change events resolve to the owning widget.
  std::unordered_map<const Node*, Widget*> widgetByRoot;
  Widget* hoverHighlightWidget = nullptr;

  Signal<>::ScopedConnection paletteConn;
  std::optional<AttachedPanelGeometry> attachedPanelGeometry;
};
