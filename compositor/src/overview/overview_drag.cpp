#include "overview/overview_internal.h"

namespace andromeda {

  using namespace ovimpl;

  // -: drag

  void Overview::beginDrag() {
    Card* card = m_pressCard;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      return;
    }
    View* view = card->view;
    m_dragCard = card;
    m_dragOffsetX = m_pressX - card->box.x;
    m_dragOffsetY = m_pressY - card->box.y;
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

  void Overview::updateDrag(double lx, double ly) {
    Card* card = m_dragCard;
    card->box.x = static_cast<int>(std::lround(lx - m_dragOffsetX));
    card->box.y = static_cast<int>(std::lround(ly - m_dragOffsetY));
    wlr_scene_node_set_position(&card->tree->node, card->box.x, card->box.y);
    m_dragX = lx;
    m_dragY = ly;
    retargetDrop(lx, ly);
    updateEdgeScroll(lx, ly);
  }

  void Overview::retargetDrop(double lx, double ly) {
    Card* card = m_dragCard;
    OutputState* state = nullptr;
    size_t workspaceIndex = 0;
    wlr_box workspaceHint{};
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

  void Overview::endDrag(bool drop) {
    Card* card = m_dragCard;
    if (card == nullptr) {
      return;
    }
    View* view = card->view;
    WorkspaceGroup* insertionGroup = drop ? m_dropWorkspaceGroup : nullptr;
    View* swapWith = drop && insertionGroup == nullptr ? swapTargetAt(m_dragX, m_dragY) : nullptr;
    Workspace* target = drop ? m_drop.workspace : nullptr;
    DropTarget targetDrop = m_drop;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;

    m_dragCard = nullptr;
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    hideDropHint();
    stopEdgeScroll();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    bool insertedWorkspace = false;
    if (insertionGroup != nullptr) {
      target = insertionGroup->insertDynamicWorkspace(m_dropWorkspaceIndex);
      dropState = target != nullptr ? stateForWorkspace(target) : nullptr;
      if (target != nullptr && dropState != nullptr) {
        targetDrop = {
            .workspace = target,
            .column = 0,
        };
        insertedWorkspace = true;
      }
    }

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
    } else if (target != nullptr && dropState != nullptr && insertedWorkspace) {
      view->rememberFloatingPosition();
      if (view->workspace() != target) {
        view->moveToWorkspace(target);
      }
      if (view->floating()) {
        view->restoreFloatingPosition();
      }
      m_server->focusView(view, FocusReason::DragDrop);
    } else if (target != nullptr && dropState != nullptr) {
      // Floating: map the card origin back out of the thumbnail.
      {
        const ov::Fit fit = workspaceFit(*dropState, target->index());
        const int x = dropState->outputBox.x + static_cast<int>(std::lround((cardBox.x - fit.ox) / fit.s));
        const int y = dropState->outputBox.y + static_cast<int>(std::lround((cardBox.y - fit.oy) / fit.s));
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
      }
    } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
      // Cancelled or dropped on nothing: put the tile back where it came from.
      restoreDragSlot(*view);
      m_dragSourceWorkspace->arrange(false);
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
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
    if (m_dragCard == nullptr || m_dragSourceWorkspace == nullptr || m_dragSourceColumn < 0) {
      return nullptr;
    }
    const Card* card = cardAt(lx, ly, m_dragCard);
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
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
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
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
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
      if (m_dragCard != nullptr) {
        wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
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
    if ((self->m_dragCard == nullptr && !self->m_thumbDragging) || state == nullptr || !self->interactive()) {
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
