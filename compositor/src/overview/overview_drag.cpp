#include "design_tokens.h"
#include "overview/overview_internal.h"

namespace andromeda {

  using namespace ovimpl;

  // -: drag

  void Overview::beginDrag() {
    Card* card = m_pointer.beginDrag();
    m_pressWorkspace = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      m_pointer.release();
      return;
    }
    View* view = card->view;
    m_dragGrabX = card->box.width > 0 ? (m_pressX - card->box.x) / card->box.width : 0.5;
    m_dragGrabY = card->box.height > 0 ? (m_pressY - card->box.y) / card->box.height : 0.5;
    m_dragSlotScale = card->scale;
    m_dragScale = card->scale;
    m_dragClaimX = m_dragX;
    m_dragClaimY = m_dragY;
    m_dragSamples.clear();
    m_dropThumb = nullptr;
    if (m_return.has_value() && m_return->view.get() == view) {
      m_return.reset();
    }
    m_dragSourceWorkspace = view->workspace();
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    m_dragSourcePromotes = false;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      m_dragSourceWidth = captureDropColumnWidth(*m_dragSourceWorkspace, view);
      const MasterStackLayout* master = m_dragSourceWorkspace->masterLayout();
      m_dragSourcePromotes = master != nullptr && master->isSoleMaster(view) && master->columns().size() > 1;
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->tree->node, m_tree);
    wlr_scene_node_raise_to_top(&card->tree->node);
    if (card->owner != nullptr) {
      layoutCard(*card, liveTargetView());
    }
    m_server->cursor()->overrideCursor("grabbing");
  }

  void Overview::updateDrag(double lx, double ly, uint32_t timeMsec) {
    Card* card = dragCard();
    m_dragX = lx;
    m_dragY = ly;
    std::erase_if(m_dragSamples, [timeMsec](const DragSample& sample) {
      return static_cast<double>(timeMsec - sample.msec) > kReleaseWindowMs;
    });
    m_dragSamples.push_back({timeMsec, lx, ly});
    m_dragScale = dragScaleAt(*card, lx, ly);
    placeDragCard(*card);
    layoutCard(*card, liveTargetView());
    retargetDrop(lx, ly);
    updateEdgeScroll(lx, ly);
  }

  double Overview::dragScaleAt(const Card& card, double lx, double ly) const {
    const auto smoothstep = [](double x) {
      x = std::clamp(x, 0.0, 1.0);
      return x * x * (3.0 - 2.0 * x);
    };
    const double lift = kDragLiftShare * m_dragSlotScale;
    const double lifted =
        std::lerp(m_dragSlotScale, lift, smoothstep(std::hypot(lx - m_dragClaimX, ly - m_dragClaimY) / kDragThreshold));
    // The thumb row of the card's output: its thumbs' vertical extent and the scale a window has inside one.
    const OutputState* state = card.owner;
    if (state == nullptr) {
      return lifted;
    }
    double top = std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    double thumbScale = 0.0;
    for (const WorkspaceView& view : state->workspaceViews) {
      if (!view.shown || view.kind != ov::CardKind::Thumb) {
        continue;
      }
      top = std::min(top, static_cast<double>(view.box.y));
      bottom = std::max(bottom, static_cast<double>(view.box.y + view.box.height));
      thumbScale = view.fit.s;
    }
    if (thumbScale <= 0.0) {
      return lifted;
    }
    // The strip's near edge, above or below where the drag began.
    const double edge = m_dragClaimY > bottom ? bottom : top;
    const bool inBand = ly >= top && ly <= bottom;
    const double span = m_dragClaimY - edge;
    const double p = inBand || std::abs(span) < 1.0 ? 1.0 : (m_dragClaimY - ly) / span;
    return std::lerp(lifted, thumbScale, smoothstep(p));
  }

  void Overview::placeDragCard(Card& card) {
    const wlr_box& world = card.view->presentedBox();
    card.scale = m_dragScale;
    card.box.width = std::max(1, static_cast<int>(std::lround(world.width * m_dragScale)));
    card.box.height = std::max(1, static_cast<int>(std::lround(world.height * m_dragScale)));
    card.box.x = static_cast<int>(std::lround(m_dragX - m_dragGrabX * card.box.width));
    card.box.y = static_cast<int>(std::lround(m_dragY - m_dragGrabY * card.box.height));
  }

  Workspace* Overview::rowThumbAt(double lx, double ly, wlr_box* outBox) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !boxContains(state->outputBox, lx, ly)) {
        continue;
      }
      for (size_t index = 0; index < state->workspaceViews.size(); ++index) {
        const WorkspaceView& view = state->workspaceViews[index];
        if (view.shown && view.kind == ov::CardKind::Thumb && boxContains(view.box, lx, ly)) {
          *outBox = view.box;
          return group->workspaceAt(index);
        }
      }
    }
    return nullptr;
  }

  void Overview::retargetDrop(double lx, double ly) {
    Card* card = dragCard();
    OutputState* state = nullptr;
    size_t workspaceIndex = 0;
    wlr_box workspaceHint{};
    m_dropThumb = nullptr;
    if (WorkspaceGroup* group = plusTargetAt(lx, ly, &state, &workspaceHint)) {
      m_drop = {};
      m_dropWorkspaceGroup = group;
      m_dropWorkspaceIndex = group->workspaceCount();
      if (m_dropHint == nullptr) {
        m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      m_dropHint->show(state->output, workspaceHint, kPlusHintRadius, HintLook::Ring);
      wlr_scene_node_raise_to_top(&card->tree->node);
      scheduleFrames();
      return;
    }
    if (WorkspaceGroup* group = workspaceGapAt(lx, ly, &state, &workspaceIndex, &workspaceHint)) {
      m_drop = {};
      m_dropWorkspaceGroup = group;
      m_dropWorkspaceIndex = workspaceIndex;
      showWorkspaceInsertHint(state->output, workspaceHint);
      scheduleFrames();
      return;
    }
    m_dropWorkspaceGroup = nullptr;

    // A thumb is one target, "move there": the window joins that workspace the way the window menu moves it.
    // The dragged window's own thumb takes no drop: a release there returns the card to its slot.
    if (Workspace* thumb = rowThumbAt(lx, ly, &workspaceHint)) {
      m_drop = {};
      if (thumb == card->view->workspace()) {
        hideDropHint();
      } else {
        m_dropThumb = thumb;
        if (m_dropHint == nullptr) {
          m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
        }
        m_dropHint->show(thumb->group()->output(), workspaceHint, kPlusHintRadius, HintLook::Ring);
        wlr_scene_node_raise_to_top(&card->tree->node);
      }
      scheduleFrames();
      return;
    }

    if (View* other = swapTargetAt(lx, ly)) {
      m_drop = {.workspace = other->workspace()};
      const Card* target = cardAt(lx, ly, card);
      if (m_dropHint == nullptr) {
        m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      m_dropHint->show(
          target->owner->output, target->box,
          static_cast<int>(std::lround(config().appearance.cornerRadius * target->scale)), HintLook::Ring
      );
      wlr_scene_node_raise_to_top(&card->tree->node);
      scheduleFrames();
      return;
    }

    size_t workspacePosition = 0;
    Workspace* workspace = workspaceAtPoint(lx, ly, &state, &workspacePosition, true);
    m_drop = {.workspace = workspace};
    if (workspace == nullptr || state == nullptr) {
      hideDropHint();
      scheduleFrames();
      return;
    }

    // Map the pointer out of the card (or the main area) and back into workspace world space.
    const ov::Fit fit = workspaceFit(*state, workspacePosition);
    const double worldX = state->outputBox.x + (lx - fit.ox) / fit.s;
    const double worldY = state->outputBox.y + (ly - fit.oy) / fit.s;

    if (card->view->tiled()) {
      // The overview applies its own projection after target selection. Keep
      // strip hints attached to content that is outside the normal viewport but
      // visible in the overview margin. Other layouts retain their normal
      // usable-area bounds.
      const bool scrolling = workspace->scrollingLayout() != nullptr;
      m_drop = computeDropTarget(
          *workspace, worldX, worldY, card->view,
          DropTargetOptions{
              .clipHintToUsable = !scrolling,
              .reserveScrollingViewportEdges = false,
              .endpointGapsOutsideColumns = scrolling,
          }
      );
    } else {
      m_drop = {
          .workspace = workspace,
          .column = static_cast<int>(workspace->layout().columns().size()),
      };
    }
    if (m_drop.hintBox.width > 0 && m_drop.hintBox.height > 0) {
      showDropHint(m_drop.hintBox, *state, workspacePosition);
    } else {
      hideDropHint();
    }
    scheduleFrames();
  }

  void Overview::endDrag(bool drop, uint32_t releaseMsec) {
    Card* card = dragCard();
    if (card == nullptr) {
      return;
    }
    View* view = card->view;
    WorkspaceGroup* insertionGroup = drop ? m_dropWorkspaceGroup : nullptr;
    Workspace* thumb = drop ? m_dropThumb.get() : nullptr;
    View* swapWith = drop && insertionGroup == nullptr && thumb == nullptr ? swapTargetAt(m_dragX, m_dragY) : nullptr;
    Workspace* target = drop ? m_drop.workspace : nullptr;
    DropTarget targetDrop = m_drop;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;
    const ov::Rect released{
        static_cast<double>(card->box.x), static_cast<double>(card->box.y), static_cast<double>(card->box.width),
        static_cast<double>(card->box.height)
    };

    m_pointer.release();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_dropThumb = nullptr;
    hideDropHint();
    stopEdgeScroll();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    // Release velocity: a least-squares line through the last kReleaseWindowMs of samples (rulebook 7.7).
    double vx = 0;
    double vy = 0;
    std::erase_if(m_dragSamples, [releaseMsec](const DragSample& sample) {
      return releaseMsec == 0 || static_cast<double>(releaseMsec - sample.msec) > kReleaseWindowMs;
    });
    if (m_dragSamples.size() >= 2) {
      const auto count = static_cast<double>(m_dragSamples.size());
      double meanT = 0, meanX = 0, meanY = 0;
      for (const DragSample& sample : m_dragSamples) {
        meanT += static_cast<double>(sample.msec - m_dragSamples.front().msec) / count;
        meanX += sample.x / count;
        meanY += sample.y / count;
      }
      double stt = 0, stx = 0, sty = 0;
      for (const DragSample& sample : m_dragSamples) {
        const double dt = static_cast<double>(sample.msec - m_dragSamples.front().msec) - meanT;
        stt += dt * dt;
        stx += dt * (sample.x - meanX);
        sty += dt * (sample.y - meanY);
      }
      if (stt > 0) {
        vx = stx / stt * 1000.0;
        vy = sty / stt * 1000.0;
      }
    }
    m_dragSamples.clear();

    // Into a thumb or a new workspace, and a cancel: Return on `close`. Anywhere else: Return on `page`, carrying the
    // release velocity.
    const bool intoWorkspace = thumb != nullptr || insertionGroup != nullptr;
    const SpringConfig closeSpring{.damping = Tokens::springClose.damping, .stiffness = Tokens::springClose.stiffness};
    const SpringConfig pageSpring{.damping = Tokens::springPage.damping, .stiffness = Tokens::springPage.stiffness};
    const bool page = drop && !intoWorkspace;
    startDropReturn(*view, released, page ? pageSpring : closeSpring, page ? vx : 0.0, page ? vy : 0.0);

    if (intoWorkspace) {
      // The same move as the window menu's "Move to": the window keeps its place policy and its floating position,
      // and the overview stays on the workspace it shows.
      Workspace* destination = thumb != nullptr ? thumb : insertionGroup->insertDynamicWorkspace(m_dropWorkspaceIndex);
      if (destination != nullptr) {
        moveViewTo(*view, destination);
      } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
        restoreDragSlot(*view);
        m_dragSourceWorkspace->arrange(false);
      }
    } else {
      if (swapWith == nullptr && target != nullptr && target->layoutMode() == LayoutMode::Floating && view->tiled()) {
        view->floatForLayout();
      }
      if (swapWith != nullptr) {
        swapDragged(*view, *swapWith);
      } else if (target != nullptr && view->tiled()) {
        applyDrop(
            *m_server, *view, *target, targetDrop, m_dragSourceWidth.has_value() ? &*m_dragSourceWidth : nullptr,
            /*animate=*/false
        );
      } else if (target != nullptr && dropState != nullptr) {
        // Floating: the window goes where the grab point was let go, at its own size.
        const ov::Fit fit = workspaceFit(*dropState, target->index());
        const wlr_box& world = view->presentedBox();
        const int x = dropState->outputBox.x
            + static_cast<int>(std::lround((m_dragX - fit.ox) / fit.s - m_dragGrabX * world.width));
        const int y = dropState->outputBox.y
            + static_cast<int>(std::lround((m_dragY - fit.oy) / fit.s - m_dragGrabY * world.height));
        if (view->workspace() != target) {
          view->moveToWorkspace(target);
          target->exitFullscreenForIncomingView(view);
        }
        if (view->floating()) {
          view->setPosition(x, y);
          view->rememberFloatingPosition();
        }
        // Cards are laid out from the presented box, which setPosition does not refresh.
        target->syncViewPresentation(view);
      } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
        // Cancelled or dropped on nothing: put the tile back where it came from.
        restoreDragSlot(*view);
        m_dragSourceWorkspace->arrange(false);
      }
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
    // A card the layout did not place (hidden, filtered out) has no path to travel.
    if (m_return.has_value() && !m_return->started) {
      m_return.reset();
    }
  }

  void Overview::startDropReturn(View& view, const ov::Rect& from, const SpringConfig& spring, double vx, double vy) {
    const auto& animation = config().animation;
    const bool fade = config().appearance.reduceMotion;
    if (!animation.enabled || (!fade && !animation.overview.enabled)) {
      m_return.reset();
      return;
    }
    const SpringConfig fadeSpring{.damping = Tokens::springFade.damping, .stiffness = Tokens::springFade.stiffness};
    m_return =
        DropReturn{.view = &view, .from = from, .spring = fade ? fadeSpring : spring, .vx = vx, .vy = vy, .fade = fade};
  }

  float Overview::returnOpacity(const View* view) const {
    if (!m_return.has_value() || !m_return->fade || !m_return->started || m_return->view.get() != view) {
      return 1.0F;
    }
    // Out at the release point over the first half, in at the slot over the second.
    return static_cast<float>(std::clamp(std::abs(1.0 - 2.0 * m_return->t.current()), 0.0, 1.0));
  }

  void Overview::restoreDragSlot(View& view) {
    Workspace& source = *m_dragSourceWorkspace;
    if (source.layoutMode() == LayoutMode::Floating) {
      source.layoutAttach(&view);
    } else if (m_dragSourceWidth.has_value()) {
      source.layout().insertView(&view, m_dragSourceColumn);
      const int column = source.layout().columnOf(&view);
      if (ColumnOps* ops = source.layout().columnOps()) {
        ops->setWidthFraction(column, m_dragSourceWidth->fraction);
        if (m_dragSourceWidth->fullWidth) {
          ops->toggleFullWidth(column);
        }
      }
      view.setMaximizedState(m_dragSourceWidth->fullWidth);
    } else if (source.dwindleLayout() != nullptr) {
      // Gap-index insert restores the exact flat position the drag removed.
      source.layout().insertView(&view, m_dragSourceColumn);
    } else if (m_dragSourceRow >= 0) {
      if (ColumnOps* ops = source.layout().columnOps()) {
        ops->insertViewIntoColumn(&view, m_dragSourceColumn, m_dragSourceRow);
      }
      // Lifting the only master window promoted the first stack window; send it back to the stack top.
      if (MasterStackLayout* master = source.masterLayout(); master != nullptr && m_dragSourcePromotes) {
        master->demoteToStack();
      }
    } else {
      source.layout().insertView(&view, m_dragSourceColumn);
    }
  }

  View* Overview::swapTargetAt(double lx, double ly) {
    if (dragCard() == nullptr || m_dragSourceWorkspace == nullptr || m_dragSourceColumn < 0) {
      return nullptr;
    }
    const Card* card = cardAt(lx, ly, dragCard());
    if (card == nullptr
        || card->view == nullptr
        || !card->view->mapped()
        || !card->view->tiled()
        || card->box.width <= 0) {
      return nullptr;
    }
    const Workspace* workspace = card->view->workspace();
    if (workspace == nullptr || workspace->layoutMode() == LayoutMode::Floating) {
      return nullptr;
    }
    // A thumb is one target, "move here", as in the prototype: its miniature windows take no window drops.
    if (cardInThumb(*card)) {
      return nullptr;
    }
    const auto central = [](double share) { return share >= kSwapZoneStart && share <= 1.0 - kSwapZoneStart; };
    const bool inside = card->box.height > 0
        && central((lx - card->box.x) / card->box.width)
        && central((ly - card->box.y) / card->box.height);
    return inside ? card->view : nullptr;
  }

  // Puts the dragged `view` where `other` is and `other` into the slot the drag took `view` from.
  void Overview::swapDragged(View& view, View& other) {
    Workspace& target = *other.workspace();
    // The drag left the source empty; a dynamic group would reap it before `other` arrives.
    const std::shared_ptr<WorkspaceLaunchAnchor> keepSource = m_dragSourceWorkspace->reserveForLaunch();
    DropTarget beside{.workspace = &target};
    if (target.dwindleLayout() != nullptr) {
      beside.view = &other;
      beside.edge = WLR_EDGE_RIGHT;
    } else {
      beside.column = target.layout().columnOf(&other) + 1;
    }
    applyDrop(*m_server, view, target, beside, nullptr, /*animate=*/false);
    target.layout().swapViews(&view, &other);
    target.layoutDetach(&other, false);
    if (&target != m_dragSourceWorkspace.get()) {
      other.moveToWorkspace(m_dragSourceWorkspace.get(), /*attachToLayout=*/false);
      m_dragSourceWorkspace->exitFullscreenForIncomingView(&other);
    }
    restoreDragSlot(other);
    other.setSceneParent(m_dragSourceWorkspace->viewLayer(true));
    // Each window takes the full-width state of the column it now sits in.
    const auto fullWidth = [](Workspace& workspace, const View& member) {
      ColumnOps* ops = workspace.layout().columnOps();
      const int column = workspace.layout().columnOf(&member);
      return ops != nullptr && column >= 0 && ops->isFullWidth(column);
    };
    view.setMaximizedState(fullWidth(target, view));
    other.setMaximizedState(fullWidth(*m_dragSourceWorkspace, other));
    target.arrange(false);
    m_dragSourceWorkspace->arrange(false);
    m_server->focusView(&view, FocusReason::DragDrop);
  }

  bool Overview::cardInThumb(const Card& card) const {
    if (card.owner == nullptr) {
      return false;
    }
    const OutputState& state = *card.owner;
    const bool inMain = state.mainBox.has_value() && card.workspaceIndex == state.mainWorkspace;
    return !inMain
        && card.workspaceIndex < state.workspaceViews.size()
        && state.workspaceViews[card.workspaceIndex].kind == ov::CardKind::Thumb;
  }

  Workspace* Overview::thumbAt(double lx, double ly, wlr_box* outBox) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !boxContains(state->outputBox, lx, ly)) {
        continue;
      }
      for (size_t index = 0; index < state->workspaceViews.size(); ++index) {
        const WorkspaceView& view = state->workspaceViews[index];
        if (view.shown && view.kind != ov::CardKind::Frame && boxContains(view.box, lx, ly)) {
          if (outBox != nullptr) {
            *outBox = view.box;
          }
          return group->workspaceAt(index);
        }
      }
    }
    return nullptr;
  }

  Workspace* Overview::thumbDropAt(double lx, double ly, wlr_box* outBox) {
    Workspace* target = thumbAt(lx, ly, outBox);
    const Workspace* source = m_pressWorkspace.get();
    return target != nullptr && source != nullptr && target != source && target->group() == source->group() ? target
                                                                                                            : nullptr;
  }

  void Overview::updateThumbDrag(double lx, double ly) {
    m_dragX = lx;
    m_dragY = ly;
    updateEdgeScroll(lx, ly);
    wlr_box box{};
    Workspace* target = thumbDropAt(lx, ly, &box);
    if (target == nullptr || target->group() == nullptr) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(target->group()->output(), box, kPlusHintRadius, HintLook::Ring);
    scheduleFrames();
  }

  void Overview::endThumbDrag(bool drop, double lx, double ly) {
    if (!m_thumbDragging) {
      m_pressThumb = false;
      return;
    }
    Workspace* target = drop ? thumbDropAt(lx, ly, nullptr) : nullptr;
    Workspace* source = m_pressWorkspace.get();
    m_thumbDragging = false;
    m_pressThumb = false;
    m_pressWorkspace = nullptr;
    hideDropHint();
    stopEdgeScroll();
    m_server->cursor()->overrideCursor(nullptr);
    if (target != nullptr && source != nullptr) {
      source->group()->moveWorkspace(source, target->index());
    }
  }

  void Overview::syncWorkspaceRows(OutputState& state, WorkspaceGroup& group) {
    const bool grew = state.workspaceBackgrounds.size() < group.workspaceCount();
    while (state.workspaceBackgrounds.size() > group.workspaceCount()) {
      WorkspaceBackground background = std::move(state.workspaceBackgrounds.back());
      state.workspaceBackgrounds.pop_back();
      for (const auto& mirror : background.mirrors) {
        mirror->outputSample.disconnect();
        mirror->frameDone.disconnect();
      }
      if (background.tree != nullptr) {
        wlr_scene_node_destroy(&background.tree->node);
      }
    }
    while (state.workspaceBackgrounds.size() < group.workspaceCount()) {
      state.workspaceBackgrounds.push_back(createWorkspaceBackground(state));
    }
    if (grew) {
      syncDesktopMirrors(state);
    }
    for (const auto& card : state.cards) {
      if (card->view != nullptr && card->view->workspace() != nullptr) {
        card->workspaceIndex = card->view->workspace()->index();
      }
    }
  }

  void Overview::showDropHint(const wlr_box& worldBox, const OutputState& state, size_t workspaceIndex) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    const ov::Fit fit = workspaceFit(state, workspaceIndex);
    const double z = fit.s;
    const wlr_box mappedBox{
        .x = static_cast<int>(std::lround(fit.ox + (worldBox.x - state.outputBox.x) * z)),
        .y = static_cast<int>(std::lround(fit.oy + (worldBox.y - state.outputBox.y) * z)),
        .width = std::max(1, static_cast<int>(std::lround(worldBox.width * z))),
        .height = std::max(1, static_cast<int>(std::lround(worldBox.height * z))),
    };
    Output* output = state.output;
    wlr_box visibleBox{};
    if (!wlr_box_intersection(&visibleBox, &mappedBox, &state.outputBox)) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(
        output, visibleBox, static_cast<int>(std::lround(config().appearance.cornerRadius * z)), HintLook::Ring
    );
    if (dragCard() != nullptr && dragCard()->tree != nullptr) {
      wlr_scene_node_raise_to_top(&dragCard()->tree->node);
    }
  }

  void Overview::showWorkspaceInsertHint(Output* output, const wlr_box& box) {
    if (output == nullptr || box.width <= 0 || box.height <= 0) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(output, box, std::min(box.width, box.height) / 2, HintLook::Glow);
    if (dragCard() != nullptr && dragCard()->tree != nullptr) {
      wlr_scene_node_raise_to_top(&dragCard()->tree->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      m_dropHint->hideImmediate();
    }
  }

  WorkspaceGroup* Overview::plusTargetAt(double lx, double ly, OutputState** outState, wlr_box* outBox) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (!state->placement.plus.has_value()
          || group == nullptr
          || !group->dynamic()
          || group->workspaceCount() >= kMaxWorkspaces) {
        continue;
      }
      const ov::Rect& plus = *state->placement.plus;
      const wlr_box box{
          state->outputBox.x + static_cast<int>(std::lround(plus.x)),
          state->outputBox.y + static_cast<int>(std::lround(plus.y)),
          static_cast<int>(std::lround(plus.w)),
          static_cast<int>(std::lround(plus.h)),
      };
      wlr_box visible{};
      if (wlr_box_intersection(&visible, &box, &state->outputBox) && wlr_box_contains_point(&visible, lx, ly)) {
        *outState = state.get();
        *outBox = visible;
        return group;
      }
    }
    return nullptr;
  }

  void Overview::updateEdgeScroll(double lx, double ly) {
    Output* output = nullptr;
    int step = 0;
    int pan = 0;
    wlr_box edge{};
    wlr_edges strong = WLR_EDGE_NONE;
    OutputState* state = nullptr;
    for (const auto& candidate : m_outputs) {
      if (wlr_box_contains_point(&candidate->outputBox, lx, ly)) {
        state = candidate.get();
        break;
      }
    }
    Workspace* panned = nullptr;
    const ov::Style style = configuredStyle();
    WorkspaceGroup* group = state != nullptr ? state->output->workspaceGroup() : nullptr;
    if (group != nullptr && (style == ov::Style::Strip || style == ov::Style::Spaces)) {
      const wlr_box& box = state->outputBox;
      const bool horizontal = state->cardsHorizontal;
      const int bar = static_cast<int>(kEdgeZone);
      const double x = lx - box.x;
      const double y = ly - box.y;
      // Strip rows step along the workspace axis; the prototype has no row steps in the other styles.
      if (style == ov::Style::Strip) {
        const double along = horizontal ? x : y;
        const double alongExtent = horizontal ? box.width : box.height;
        if (along < kEdgeZone) {
          step = -1;
          edge = horizontal ? wlr_box{box.x, box.y, bar, box.height} : wlr_box{box.x, box.y, box.width, bar};
          strong = horizontal ? WLR_EDGE_LEFT : WLR_EDGE_TOP;
        } else if (along > alongExtent - kEdgeZone) {
          step = 1;
          edge = horizontal ? wlr_box{box.x + box.width - bar, box.y, bar, box.height}
                            : wlr_box{box.x, box.y + box.height - bar, box.width, bar};
          strong = horizontal ? WLR_EDGE_RIGHT : WLR_EDGE_BOTTOM;
        }
      }
      // The left and right edges pan a horizontal scrolling workspace: the Strip row under the pointer, or the
      // Spaces main area.
      const bool panAxisFree = style == ov::Style::Spaces || !horizontal;
      if (step == 0 && panAxisFree && (x < kEdgeZone || x > box.width - kEdgeZone)) {
        Workspace* workspace = nullptr;
        if (style == ov::Style::Spaces) {
          workspace = state->mainBox.has_value() ? group->workspaceAt(state->mainWorkspace) : nullptr;
        } else {
          size_t position = 0;
          OutputState* rowState = nullptr;
          workspace = workspaceAtPoint(lx, ly, &rowState, &position, true);
        }
        if (workspace != nullptr && workspace->scrollingLayout() != nullptr && !workspace->scrollingVertical()) {
          pan = x < kEdgeZone ? -1 : 1;
          panned = workspace;
          edge = pan < 0 ? wlr_box{box.x, box.y, bar, box.height}
                         : wlr_box{box.x + box.width - bar, box.y, bar, box.height};
          strong = pan < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
        }
      }
      if (step != 0 || pan != 0) {
        output = state->output;
      }
    }
    m_edgeWorkspace = panned;
    if (output == m_edgeOutput && step == m_edgeStep && pan == m_edgePan) {
      return;
    }
    m_edgeOutput = output;
    m_edgeStep = step;
    m_edgePan = pan;
    if (output == nullptr) {
      if (m_edgeTimer != nullptr) {
        wl_event_source_timer_update(m_edgeTimer, 0);
      }
      if (m_edgeHint != nullptr) {
        m_edgeHint->hideImmediate();
      }
    } else {
      if (m_edgeTimer == nullptr) {
        m_edgeTimer = wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onEdgeTimer, this);
      }
      wl_event_source_timer_update(m_edgeTimer, step != 0 ? kEdgeDelayMs : 0);
      m_edgePanLastMsec = -1.0;
      if (pan != 0) {
        scheduleFrames();
      }
      if (m_edgeHint == nullptr) {
        m_edgeHint = std::make_unique<HintRect>(*m_server, m_tree);
      }
      // A new edge fades in afresh, as in the prototype: the gradient must not morph across the output.
      m_edgeHint->hideImmediate();
      m_edgeHint->show(output, edge, 0, HintLook::Edge, strong);
      if (dragCard() != nullptr) {
        wlr_scene_node_raise_to_top(&dragCard()->tree->node);
      }
    }
    scheduleFrames();
  }

  void Overview::stopEdgeScroll() {
    if (m_edgeTimer != nullptr) {
      wl_event_source_timer_update(m_edgeTimer, 0);
    }
    if (m_edgeHint != nullptr) {
      m_edgeHint->hideImmediate();
    }
    const bool wasArmed = m_edgeOutput != nullptr;
    Output* previous = m_edgeOutput;
    m_edgeOutput = nullptr;
    m_edgeWorkspace = nullptr;
    m_edgeStep = 0;
    m_edgePan = 0;
    if (wasArmed) {
      if (OutputState* state = stateFor(previous); state != nullptr && state->chrome != nullptr) {
        layoutChrome(*state, liveTargetView());
      }
    }
  }

  int Overview::onEdgeTimer(void* data) {
    auto* self = static_cast<Overview*>(data);
    OutputState* state = self->m_edgeOutput != nullptr ? self->stateFor(self->m_edgeOutput) : nullptr;
    if ((self->dragCard() == nullptr && !self->m_thumbDragging) || state == nullptr || !self->interactive()) {
      self->stopEdgeScroll();
      return 0;
    }
    if (self->m_edgeStep != 0) {
      self->selectRelativeWorkspace(self->m_edgeStep, state->output);
      wl_event_source_timer_update(self->m_edgeTimer, kEdgeRepeatMs);
      return 0;
    }
    self->stopEdgeScroll();
    return 0;
  }

} // namespace andromeda
