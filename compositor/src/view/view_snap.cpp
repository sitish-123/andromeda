#include "config/resolve.h"
#include "input/cursor.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "view/view_internal.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace andromeda {
  bool View::canSnap() const {
    const ScratchpadManager* scratchpad = m_server->scratchpadManager();
    return mapped()
        && !tiled()
        && !attached()
        && !placedPinned()
        && !scheduledFullscreen()
        && !currentFullscreen()
        && (scratchpad == nullptr || !scratchpad->contains(this));
  }

  std::optional<wlr_box>
  View::floatingSnappedBox(const SnapSpot& spot, const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    const auto resolved = layout != nullptr ? *layout
        : m_workspace != nullptr            ? m_workspace->layoutConfig()
                                            : resolveGlobalLayout(config());
    const wlr_box tile = snapSpotBox(floatingMarginArea(usable, &resolved), spot, resolved.totalGap);
    // Snapping leaves maximize-to-edges, which makes the title bar visible again.
    const int bar = wantsTitleBar(titleBarModeFor(m_titleBarMode, m_workspace))
        ? config().appearance.titleBar.height
        : 0;
    if (tile.width <= 0 || tile.height <= bar) {
      return std::nullopt;
    }
    const wlr_box content = contentBoxInTile(tile, bar);
    const SizeHints hints = sizeHints();
    if (hints.minWidth > content.width || hints.minHeight > content.height) {
      return std::nullopt;
    }
    const int width = clampWidth(content.width, hints);
    const int height = clampHeight(content.height, hints);
    return wlr_box{
        .x = content.x + (content.width - width) / 2,
        .y = content.y + (content.height - height) / 2,
        .width = width,
        .height = height,
    };
  }

  bool View::snapTo(const SnapSpot& spot, std::optional<FloatingPoint> restoreOrigin) {
    if (!canSnap()) {
      return false;
    }
    if (snapSpot() == spot) {
      unsnap();
      return true;
    }
    const auto box = floatingSnappedBox(spot, floatingUsableArea());
    if (!box) {
      return false;
    }
    if (!snapSpot()) {
      const auto [width, height] = floatingSize();
      wlr_box restore = m_hasMaximizeRestoreBox
          ? m_maximizeRestoreBox
          : wlr_box{.x = layoutTargetX(), .y = layoutTargetY(), .width = width, .height = height};
      if (restoreOrigin) {
        restore.x = restoreOrigin->x;
        restore.y = restoreOrigin->y;
      }
      m_snapRestoreBox = width > 0 && height > 0 ? std::optional(restore) : std::nullopt;
    }
    dropMaximizedForResize();
    transitionPlacement({.event = PlacementEvent::Snap, .spot = spot});
    m_snappedBox = *box;
    requestFloatingSize(box->width, box->height);
    beginResizeAnimation(box->width, box->height);
    animateTo(box->x, box->y);
    syncSsdFrame();
    m_server->scheduleIpcWindowsEvent();
    return true;
  }

  void View::clearSnapState() {
    if (snapSpot()) {
      m_server->scheduleIpcWindowsEvent();
    }
    transitionPlacement({.event = PlacementEvent::Unsnap});
    m_snapRestoreBox.reset();
    m_snappedBox = {};
  }

  void View::unsnap(bool animate) {
    if (!snapSpot() || !canSnap()) {
      return;
    }
    const std::optional<wlr_box> restore = m_snapRestoreBox;
    clearSnapState();
    dropMaximizedForResize();
    if (!restore) {
      return;
    }
    requestFloatingSize(restore->width, restore->height);
    if (animate) {
      beginResizeAnimation(restore->width, restore->height);
      animateTo(restore->x, restore->y);
    } else {
      cancelSizeAnimation();
      setPosition(restore->x, restore->y);
    }
    rememberFloatingPosition();
  }

  void View::refitFloatingSnapped() {
    if (!snapSpot() || !canSnap() || scheduledMaximized() || m_server->cursor()->isDraggingView(this)) {
      return;
    }
    const auto box = floatingSnappedBox(*snapSpot(), floatingUsableArea());
    if (!box) {
      unsnap();
      return;
    }
    if (box->x == m_snappedBox.x
        && box->y == m_snappedBox.y
        && box->width == m_snappedBox.width
        && box->height == m_snappedBox.height) {
      return;
    }
    m_snappedBox = *box;
    requestFloatingSize(box->width, box->height);
    setPosition(box->x, box->y);
    syncFloatingSurfaceClip();
  }
} // namespace andromeda
