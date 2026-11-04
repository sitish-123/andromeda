#include "input/cursor.h"

#include "input/gestures.h"
#include "input/seat.h"
#include "output/output.h"
#include "scene/hint_rect.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
// clang-format off
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"


namespace andromeda {

  bool Cursor::beginMove(View* view, uint32_t button, bool deferUntilThreshold) {
    if (view == nullptr || !view->mapped() || button == 0) {
      return false;
    }
    // An attached dialog's place is its parent's, so dragging it drags the window it lives in.
    view = view->placementRoot();
    if (!isPassthrough()) {
      resetMode();
    }
    bool tiled = view->tiled();
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      view->restoreMaximizedForMove();
      view->setFloating(true);
      tiled = false;
    }

    if (!tiled && (view->snapRegion() || view->floatingMaximized())) {
      deferUntilThreshold = true;
    }
    setActiveConstraint(nullptr);
    const bool pinned = !tiled && view->pinned();
    // A pinned window is floating, but it remembers whether it was tiled when
    // it got pinned, and that is where unpinning it puts it back.
    const bool tiledUnderneath = tiled || (pinned && view->restoresTiledOnUnpin());
    MoveGrab grab{
        .view = view,
        .offsetX = m_cursor->x - view->layoutX(),
        .offsetY = m_cursor->y - view->layoutY(),
        .target = tiled ? DragTarget::Tiled : (pinned ? DragTarget::Pinned : DragTarget::Floating),
        .floatingLayout = !tiled
            && !pinned
            && view->workspace() != nullptr
            && view->workspace()->layoutMode() == LayoutMode::Floating,
        .unpinned = tiledUnderneath ? DragTarget::Tiled : DragTarget::Floating,
        .sourceWorkspace = tiled ? view->workspace() : nullptr,
        .sourceColumn = -1,
        .sourceWidth = std::nullopt,
        .drop = {},
        .snap = std::nullopt,
        .preDragX = view->layoutTargetX(),
        .preDragY = view->layoutTargetY(),
        .preDragArea = view->floatingUsableArea(),
        .pending = tiled || deferUntilThreshold,
        .startX = m_cursor->x,
        .startY = m_cursor->y,
        .lastX = m_cursor->x,
        .lastY = m_cursor->y,
    };
    if (grab.sourceWorkspace != nullptr) {
      grab.sourceColumn = grab.sourceWorkspace->layout().columnOf(view);
      grab.sourceWidth = captureDropColumnWidth(*grab.sourceWorkspace, view);
      grab.drop = {
          .workspace = grab.sourceWorkspace,
          .column = std::max(0, grab.sourceColumn),
      };
    }
    m_grab = grab;
    m_grabButton = button;
    if (!grab.pending) {
      view->enterDragPresentation();
      std::get<MoveGrab>(m_grab).physics = view->beginDragPhysics(grab.offsetX, grab.offsetY);
    }
    updateInteractiveCursor(view);
    return true;
  }

  bool Cursor::beginResize(View* view, uint32_t edges, uint32_t button) {
    if (view == nullptr || !view->mapped() || button == 0) {
      return false;
    }
    if (!isPassthrough()) {
      resetMode();
    }
    bool tiled = view->tiled();
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && !view->attached() && scratchpad->contains(view)) {
      view->setFloating(true);
      tiled = false;
    }

    if (tiled) {
      Workspace* workspace = view->workspace();
      if (workspace == nullptr || workspace->group() == nullptr || workspace->group()->output() == nullptr) {
        refreshInteractiveCursor();
        return false;
      }
      Layout& layout = workspace->layout();
      uint32_t resolvedEdges = 0;
      if (edges != 0) {
        resolvedEdges = layout.resolveResizeEdges(view, edges, m_cursor->x, m_cursor->y);
      } else {
        resolvedEdges = tiledResizeEdges(view, m_cursor->x, m_cursor->y);
      }
      if (resolvedEdges == 0) {
        revealClickedTile(view);
        refreshInteractiveCursor();
        return false;
      }
      setActiveConstraint(nullptr);
      if (view->maximizedToEdges()) {
        view->setMaximizedToEdges(false, false);
      }
      const wlr_box usable = workspace->tiledArea();
      std::unique_ptr<ResizeGrab> session = layout.beginResize(view, resolvedEdges, usable);
      if (session == nullptr) {
        refreshInteractiveCursor();
        return false;
      }
      if (session->unmaximizeOnBegin()) {
        view->setMaximizedState(false);
      }
      m_grab = TiledResizeGrab{
          .view = view,
          .workspace = workspace,
          .startX = m_cursor->x,
          .startY = m_cursor->y,
          .edges = resolvedEdges,
          .session = std::move(session),
      };
      m_grabButton = button;
      updateInteractiveCursor(view);
      return true;
    }
    if (edges == 0) {
      refreshInteractiveCursor();
      return false;
    }
    setActiveConstraint(nullptr);
    if (view->maximizedToEdges()) {
      view->setMaximizedToEdges(false, false);
    }

    const wlr_box& geometry = view->geometryBox();
    const double borderX = (view->layoutX() + geometry.x) + ((edges & WLR_EDGE_RIGHT) != 0 ? geometry.width : 0);
    const double borderY = (view->layoutY() + geometry.y) + ((edges & WLR_EDGE_BOTTOM) != 0 ? geometry.height : 0);
    m_grab = FloatingResizeGrab{
        .view = view,
        .offsetX = m_cursor->x - borderX,
        .offsetY = m_cursor->y - borderY,
        .geometryX = geometry.x + view->layoutX(),
        .geometryY = geometry.y + view->layoutY(),
        .geometryWidth = geometry.width,
        .geometryHeight = geometry.height,
        .edges = edges,
    };
    m_grabButton = button;
    view->beginFloatingResize(edges);
    updateInteractiveCursor(view);
    return true;
  }

  void Cursor::resetMode() {
    clearSnapPreview();
    m_server->hideInsertHint();
    cancelDataDragEdgeScroll();
    View* view = grabbedView();
    if (std::holds_alternative<ScrollDragGrab>(m_grab)) {
      m_server->gestures()->endPointerScroll(true, 0);
    }
    const bool restoreDragPresentation = isDraggingView(view);
    if (auto* grab = std::get_if<MoveGrab>(&m_grab); grab != nullptr && grab->view != nullptr && grab->physics) {
      grab->view->endDragPhysics();
    }
    const auto* tiledResize = std::get_if<TiledResizeGrab>(&m_grab);
    Workspace* resizedWorkspace = tiledResize != nullptr ? tiledResize->workspace : nullptr;
    const bool restoreResizePresentation = std::holds_alternative<FloatingResizeGrab>(m_grab);
    if (std::holds_alternative<FloatingResizeGrab>(m_grab) && view != nullptr) {
      view->finishFloatingResize();
    }
    m_toplevelDrag = false;
    m_grab = PassthroughGrab{};
    m_grabButton = 0;
    if (restoreDragPresentation && view != nullptr) {
      view->restoreHomePresentation();
      // A drag abandoned outside its drop path (session lock mid-drag, the
      // toplevel-drag detach endings) never runs finishFloatMove. Settle here
      // exactly like a drop: nothing else re-clamps where the drag parked the
      // window, and the margin must hold without waiting for a usable change.
      if (view->floating()) {
        view->settleFloatingDrop(view->layoutX(), view->layoutY());
      }
    }
    // Resize scaling belongs to the grab, including every sibling tile it resizes. Clear it before restoring clips:
    // an unchanged clip otherwise keeps the scaled source/destination until the client commits again.
    if (resizedWorkspace != nullptr) {
      for (View* resized : resizedWorkspace->allViews()) {
        if (resized->mapped() && resized->tiled()) {
          resized->resetPresentedSurface();
          resized->syncOwnedPresentation();
        }
      }
    } else if (restoreResizePresentation && view != nullptr && view->mapped()) {
      view->resetPresentedSurface();
      view->syncOwnedPresentation();
    }
    refreshInteractiveCursor();
  }

  void Cursor::updateSnapPreview() {
    auto* grab = std::get_if<MoveGrab>(&m_grab);
    if (grab == nullptr
        || grab->pending
        || grab->target != DragTarget::Floating
        || m_toplevelDrag
        || !config().input.windowSnapping
        || m_server->seat()->wlr()->drag != nullptr
        || !grab->view->canSnap()) {
      clearSnapPreview();
      return;
    }
    Output* output =
        m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y));
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      clearSnapPreview();
      return;
    }
    const wlr_box area = output->layoutBox();
    uint32_t shared = 0;
    const auto adjacent = [&](double x, double y) {
      wlr_output* other = wlr_output_layout_output_at(m_server->outputLayout(), x, y);
      return other != nullptr && other != output->wlr();
    };
    if (m_cursor->x - area.x < kSnapEdge && adjacent(area.x - 1, m_cursor->y))
      shared |= WLR_EDGE_LEFT;
    if (area.x + area.width - m_cursor->x <= kSnapEdge && adjacent(area.x + area.width, m_cursor->y))
      shared |= WLR_EDGE_RIGHT;
    if (m_cursor->y - area.y < kSnapEdge && adjacent(m_cursor->x, area.y - 1))
      shared |= WLR_EDGE_TOP;
    if (area.y + area.height - m_cursor->y <= kSnapEdge && adjacent(m_cursor->x, area.y + area.height))
      shared |= WLR_EDGE_BOTTOM;
    if (grab->dwellOutput != output || grab->dwellEdges != shared) {
      if (m_snapDwellTimer != nullptr)
        wl_event_source_timer_update(m_snapDwellTimer, 0);
      grab->dwellOutput = output;
      grab->dwellEdges = shared;
      grab->dwellReady = false;
      if (shared != 0) {
        if (m_snapDwellTimer == nullptr) {
          m_snapDwellTimer =
              wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onSnapDwellTimer, this);
        }
        if (m_snapDwellTimer != nullptr)
          wl_event_source_timer_update(m_snapDwellTimer, kSnapDwellMs);
      }
    }
    const auto hit = snapZoneAt(area, m_cursor->x, m_cursor->y, grab->dwellReady ? 0 : shared);
    if (!hit) {
      grab->snap.reset();
      grab->snapOutput = nullptr;
      m_server->hideInsertHint();
      return;
    }
    const Workspace* workspace = output->workspaceGroup()->active();
    const auto* layout = workspace != nullptr ? &workspace->layoutConfig() : nullptr;
    std::optional<wlr_box> content = grab->view->floatingSnappedBox(*hit, output->usableArea(), layout);
    if (!content || content->width <= 0 || content->height <= 0) {
      grab->snap.reset();
      grab->snapOutput = nullptr;
      m_server->hideInsertHint();
      return;
    }
    const int bar = grab->view->titleBarHeight();
    content->y -= bar;
    content->height += bar;
    grab->snap = hit;
    grab->snapOutput = output;
    m_server->insertHint().show(output, *content, config().appearance.cornerRadius);
  }

  void Cursor::finishFloatMove(int x, int y) {
    const auto* grab = std::get_if<MoveGrab>(&m_grab);
    const auto snap = grab != nullptr && config().input.windowSnapping ? grab->snap : std::nullopt;
    Output* snapOutput = grab != nullptr ? grab->snapOutput : nullptr;
    const int preDragX = grab != nullptr ? grab->preDragX : x;
    const int preDragY = grab != nullptr ? grab->preDragY : y;
    const wlr_box preDragArea = grab != nullptr ? grab->preDragArea : wlr_box{};
    const bool floatingLayout = grab != nullptr && grab->floatingLayout;
    View* view = grabbedView();
    if (view == nullptr || !view->mapped()) {
      resetMode();
      return;
    }

    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      scratchpad->finishMove(view, output);
    } else if (output != nullptr && output->workspaceGroup() != nullptr) {
      if (Workspace* target = output->workspaceGroup()->active(); view->workspace() != target) {
        view->moveToWorkspace(target);
        target->exitFullscreenForIncomingView(view);
      }
    }
    // Commit the final position after any workspace transfer, including drags whose target was deferred until
    // release, so later restores use the dropped origin. A free drop settles into the grabbable margin here (animated,
    // exactly at release), since the drag itself may have parked the window mostly out of frame.
    const wlr_box usable = view->floatingUsableArea();
    const FloatingPoint settled = view->settleFloatingDrop(x, y);

    resetMode();
    if (snap && snapOutput == output && view->canSnap()) {
      FloatingGeometry normalPosition;
      normalPosition.rememberPositionFraction({preDragX, preDragY}, preDragArea);
      // The restore is a placement too: the remembered fraction can resolve outside the margin on a smaller output.
      const FloatingPoint restore = view->grabbableOrigin(
          normalPosition.restoredOrigin(usable).value_or(FloatingPoint{preDragX, preDragY}), usable
      );
      if (!floatingLayout) {
        view->setPosition(restore.x, restore.y);
      }
      if (!view->snapTo(*snap, floatingLayout ? std::optional{restore} : std::nullopt)) {
        // Client constraints can change after the preview was drawn. Keep the free drop when it no longer fits.
        view->setPosition(settled.x, settled.y);
      }
    }
    m_server->focusView(view, FocusReason::DragDrop);
  }

  void Cursor::processResize() {
    auto* grab = std::get_if<FloatingResizeGrab>(&m_grab);
    if (grab == nullptr || grab->view == nullptr) {
      resetMode();
      return;
    }
    const double borderX = m_cursor->x - grab->offsetX;
    const double borderY = m_cursor->y - grab->offsetY;
    int newLeft = grab->geometryX;
    int newRight = grab->geometryX + grab->geometryWidth;
    int newTop = grab->geometryY;
    int newBottom = grab->geometryY + grab->geometryHeight;
    const SizeHints hints = grab->view->sizeHints();

    if ((grab->edges & WLR_EDGE_TOP) != 0) {
      newTop = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newTop = newBottom - hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newTop = newBottom - hints.maxHeight;
      }
    } else if ((grab->edges & WLR_EDGE_BOTTOM) != 0) {
      newBottom = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newBottom = newTop + hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newBottom = newTop + hints.maxHeight;
      }
    }

    if ((grab->edges & WLR_EDGE_LEFT) != 0) {
      newLeft = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newLeft = newRight - hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newLeft = newRight - hints.maxWidth;
      }
    } else if ((grab->edges & WLR_EDGE_RIGHT) != 0) {
      newRight = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newRight = newLeft + hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newRight = newLeft + hints.maxWidth;
      }
    }

    grab->view->resizeFloating(newRight - newLeft, newBottom - newTop);
  }

  uint32_t Cursor::hoverResizeEdges(View* view) const {
    if (view == nullptr) {
      return 0;
    }
    // Only advertise resize when the pointer is near the edge that would be grabbed.
    constexpr double kHoverSlop = 28.0;

    if (view->floating()) {
      if (view->sceneTree() == nullptr) {
        return 0;
      }
      const wlr_box& geo = view->geometryBox();
      const double left = view->layoutX() + geo.x;
      const double top = view->layoutY() + geo.y;
      const double right = left + geo.width;
      const double bottom = top + geo.height;
      const double distLeft = std::abs(m_cursor->x - left);
      const double distRight = std::abs(m_cursor->x - right);
      const double distTop = std::abs(m_cursor->y - top);
      const double distBottom = std::abs(m_cursor->y - bottom);
      const double nearestH = std::min(distLeft, distRight);
      const double nearestV = std::min(distTop, distBottom);
      if (std::min(nearestH, nearestV) > kHoverSlop) {
        return 0;
      }
      return floatResizeEdges(view);
    }

    if (view->workspace() == nullptr) {
      return 0;
    }
    Workspace* workspace = view->workspace();
    const wlr_box box = workspace->presentedTiledBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    wlr_box reachable = box;
    if (workspace->group() != nullptr && workspace->group()->output() != nullptr) {
      const wlr_box usable = workspace->usableArea();
      if (!wlr_box_intersection(&reachable, &box, &usable)) {
        return 0;
      }
    }
    uint32_t edges =
        workspace->layout().sanitizeResizeEdges(view, resizeEdgesForPoint(reachable, m_cursor->x, m_cursor->y));
    // Advertise an edge that extends past the output from the reachable
    // output boundary, since the pointer cannot approach the real edge.
    const double left = reachable.x;
    const double right = reachable.x + reachable.width;
    const double top = reachable.y;
    const double bottom = reachable.y + reachable.height;
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      const double dist = (edges & WLR_EDGE_LEFT) != 0 ? std::abs(m_cursor->x - left) : std::abs(m_cursor->x - right);
      if (dist > kHoverSlop) {
        edges &= ~(WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
      }
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0) {
      const double dist = (edges & WLR_EDGE_TOP) != 0 ? std::abs(m_cursor->y - top) : std::abs(m_cursor->y - bottom);
      if (dist > kHoverSlop) {
        edges &= ~(WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
      }
    }
    return edges;
  }

} // namespace andromeda
