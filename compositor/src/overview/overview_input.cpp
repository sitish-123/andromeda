#include "overview/overview_internal.h"

namespace andromeda {

  using namespace ovimpl;

  // -: hit testing

  Overview::Card* Overview::cardAt(double lx, double ly, const Card* skip) {
    // Topmost first: later outputs and later cards paint over earlier ones.
    // Hit testing uses the same card and output clips as rendering. The dragged card is
    // reparented to the unclipped root and hits everywhere.
    for (const auto& state : std::views::reverse(m_outputs)) {
      for (const auto& card : std::views::reverse(state->cards)) {
        if (card.get() == skip || card->tree == nullptr || !card->tree->node.enabled) {
          continue;
        }
        wlr_box shown = card->box;
        if (card->clip.width > 0 && card->clip.height > 0) {
          shown = {card->box.x + card->clip.x, card->box.y + card->clip.y, card->clip.width, card->clip.height};
        }
        wlr_box hit = shown;
        if (card.get() != m_dragCard && !wlr_box_intersection(&hit, &shown, &state->outputBox)) {
          continue;
        }
        if (boxContains(hit, lx, ly)) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Workspace*
  Overview::workspaceAtPoint(double lx, double ly, OutputState** outState, size_t* outIndex, bool extendScrollingAxis) {
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      // A card can only own points on its own output, so one overhanging a neighbour never steals its targets.
      if (group == nullptr || !boxContains(state->outputBox, lx, ly)) {
        continue;
      }
      const bool strip = configuredStyle() == ov::Style::Strip;
      const bool horizontalWorkspaces = group->workspaceAxis() == WorkspaceAxis::Horizontal;
      for (size_t index = 0; index < state->workspaceViews.size(); ++index) {
        Workspace* workspace = group->workspaceAt(index);
        if (workspace == nullptr) {
          continue;
        }
        bool hit = workspaceViewContains(*state, index, lx, ly);
        // A Strip row's scrolling columns overhang its frame across the whole output, so drag targeting covers that
        // band too or the extreme gaps become dead zones. Background clicks keep the frame.
        if (!hit
            && extendScrollingAxis
            && strip
            && workspace->scrollingLayout() != nullptr
            && state->workspaceViews[index].shown) {
          wlr_box band = state->workspaceViews[index].box;
          if (horizontalWorkspaces) {
            band.y = state->outputBox.y;
            band.height = state->outputBox.height;
          } else {
            band.x = state->outputBox.x;
            band.width = state->outputBox.width;
          }
          hit = boxContains(band, lx, ly);
        }
        if (!hit) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        return workspace;
      }
    }
    return nullptr;
  }

  WorkspaceGroup*
  Overview::workspaceGapAt(double lx, double ly, OutputState** outState, size_t* outIndex, wlr_box* outHintBox) {
    const bool strip = configuredStyle() == ov::Style::Strip;
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr || !group->dynamic() || group->workspaceCount() >= kMaxWorkspaces) {
        continue;
      }
      // Gaps run between neighbouring cards along the way the cards step. A Grid's wrap has no gap.
      const bool horizontalWorkspaces = state->cardsHorizontal;
      const size_t previewCount = std::min(group->workspaceCount(), state->workspaceViews.size());
      for (size_t index = 0; index < previewCount; ++index) {
        if (!state->workspaceViews[index].shown) {
          continue;
        }
        const wlr_box lower = state->workspaceViews[index].box;
        // Index zero has no preceding card. In the Strip its insertion gap begins at the output edge, so dynamic
        // workspaces can also be inserted before the first row; the thumb rows keep their margins free.
        int before = 0;
        if (index == 0) {
          if (!strip) {
            continue;
          }
          before = horizontalWorkspaces ? state->outputBox.x : state->outputBox.y;
        } else {
          if (!state->workspaceViews[index - 1].shown) {
            continue;
          }
          const wlr_box upper = state->workspaceViews[index - 1].box;
          before = horizontalWorkspaces ? upper.x + upper.width : upper.y + upper.height;
        }
        const int after = horizontalWorkspaces ? lower.x : lower.y;
        if (after <= before) {
          continue;
        }
        const wlr_box gap = horizontalWorkspaces ? wlr_box{before, lower.y, after - before, lower.height}
                                                 : wlr_box{lower.x, before, lower.width, after - before};
        wlr_box visible{};
        if (!wlr_box_intersection(&visible, &gap, &state->outputBox) || !boxContains(visible, lx, ly)) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outIndex != nullptr) {
          *outIndex = index;
        }
        if (outHintBox != nullptr) {
          // A bar across the gap: thin along the workspace axis, preview-sized across it.
          if (horizontalWorkspaces) {
            const int width = std::clamp(visible.width / 3, 4, 18);
            *outHintBox = {
                .x = visible.x + (visible.width - width) / 2,
                .y = visible.y,
                .width = width,
                .height = visible.height,
            };
          } else {
            const int height = std::clamp(visible.height / 3, 4, 18);
            *outHintBox = {
                .x = visible.x,
                .y = visible.y + (visible.height - height) / 2,
                .width = visible.width,
                .height = height,
            };
          }
        }
        return group;
      }
    }
    return nullptr;
  }

  Workspace* Overview::preferredWorkspace() const {
    Output* output = m_server->outputFromWlr(m_server->preferredOutput());
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return nullptr;
    }
    return output->workspaceGroup()->active();
  }

  void Overview::clearMiddlePress() {
    if (m_middleScrolling) {
      m_server->gestures()->endPointerScroll(true, 0);
    }
    if (m_middleDragging) {
      m_server->cursor()->overrideCursor(nullptr);
    }
    m_middlePressCard = nullptr;
    m_middleOutput = nullptr;
    m_middlePressed = false;
    m_middleDragging = false;
    m_middlePanning = false;
    m_middleScrolling = false;
    m_middleAccum = 0;
  }

  // -: input

  bool Overview::handleButton(uint32_t button, bool pressed, double lx, double ly, uint32_t timeMsec) {
    if (pressed) {
      cancelNavigation();
    }
    if (!interactive()) {
      return true; // Swallow everything while zooming back in.
    }

    if (!pressed) {
      if (button == BTN_MIDDLE) {
        Card* card = m_middlePressCard;
        const bool closeCard = m_middlePressed && !m_middleDragging;
        if (m_middleScrolling) {
          // A release settles the strip; clearMiddlePress() cancels whatever is still running.
          m_server->gestures()->endPointerScroll(false, timeMsec);
          m_middleScrolling = false;
        }
        clearMiddlePress();
        if (closeCard && card != nullptr && card->view != nullptr && card->view->mapped()) {
          card->view->requestClose();
        }
        return true;
      }
      if (button != BTN_LEFT) {
        return true;
      }
      if (m_closePress != nullptr) {
        const View* pressed = std::exchange(m_closePress, nullptr);
        // Sliding off the button before letting go cancels, like any button.
        if (closeButtonAt(lx, ly) == pressed) {
          if (Card* closing = findCard(pressed); closing != nullptr && closing->view != nullptr) {
            closing->view->requestClose();
          }
        }
        return true;
      }
      if (m_dragCard != nullptr) {
        endDrag(true);
        return true;
      }
      if (m_thumbDragging) {
        endThumbDrag(true, lx, ly);
        return true;
      }
      Card* card = m_pressCard;
      Workspace* workspace = m_pressWorkspace.get();
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        closeToWorkspace(card->view->workspace(), card->view);
      } else if (workspace != nullptr) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    }

    Card* card = cardAt(lx, ly);
    if (button == BTN_MIDDLE) {
      m_middlePressCard = card;
      Workspace* workspace = card != nullptr && card->view != nullptr
          ? card->view->workspace()
          : workspaceAtPoint(lx, ly, nullptr, nullptr, false);
      m_middleOutput = workspace != nullptr && workspace->group() != nullptr ? workspace->group()->output() : nullptr;
      m_middlePressX = lx;
      m_middlePressY = ly;
      m_middleAccum = 0;
      m_middlePressed = true;
      m_middleDragging = false;
      m_middlePanning = false;
      m_middleScrolling = false;
      return true;
    }
    if (windowMenuPress(lx, ly)) {
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      return true;
    }
    if (button == BTN_RIGHT) {
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        setMenu(std::nullopt);
        openWindowMenu(*card, lx, ly);
      }
      return true;
    }
    if (button != BTN_LEFT) {
      return true;
    }

    if (m_rename.has_value()) {
      // Pressing anywhere else ends the rename and keeps the name, like a text field losing focus.
      finishRename(true);
      return true;
    }
    if (menuPress(lx, ly)) {
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      return true;
    }
    if (std::ranges::any_of(m_outputs, [&](const auto& state) {
          return state->chrome != nullptr && state->chrome->titleAt(lx, ly);
        })) {
      constexpr uint32_t kDoubleClickMsec = 400;
      const bool second = m_titlePressMsec != 0 && timeMsec - m_titlePressMsec <= kDoubleClickMsec;
      m_titlePressMsec = second ? 0 : timeMsec;
      if (second) {
        startRename();
      }
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      return true;
    }
    if (const View* close = closeButtonAt(lx, ly)) {
      m_closePress = close;
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      return true;
    }
    // A thumb's miniature windows are part of the thumb: pressing one picks up the workspace, as in the prototype.
    if (card != nullptr && cardInThumb(*card)) {
      card = nullptr;
    }
    m_pressCard = card;
    m_pressWorkspace = nullptr;
    m_pressX = lx;
    m_pressY = ly;
    m_pressThumb = false;
    if (card == nullptr) {
      m_pressWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, false);
      m_pressThumb = m_pressWorkspace != nullptr && thumbAt(lx, ly) == m_pressWorkspace.get();
    }
    return true;
  }

  void Overview::handleMotion(double lx, double ly, uint32_t timeMsec) {
    if (!interactive()) {
      return;
    }
    // The live target resolves through preferredOutput(), and the overview owns motion while it is up: repaint when
    // the pointer crosses outputs, whether by hand or through the warp a keybind performs to change output.
    if (Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
        output != m_pointerOutput) {
      m_pointerOutput = output;
      applyProgress();
    }
    if (m_middlePressed) {
      const WorkspaceGroup* group = m_middleOutput != nullptr ? m_middleOutput->workspaceGroup() : nullptr;
      const bool horizontal = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
      if (!m_middleDragging) {
        const double dx = lx - m_middlePressX;
        const double dy = ly - m_middlePressY;
        if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
          return;
        }
        m_middleDragging = true;
        // Travel across the workspace axis pans the strip; travel along it steps rows.
        m_middlePanning = (std::abs(dx) > std::abs(dy)) != horizontal;
        if (m_middlePanning) {
          m_middleScrolling = m_server->gestures()->beginPointerScroll(m_middlePressX, m_middlePressY);
          if (m_middleScrolling) {
            m_server->gestures()->updatePointerScroll(dx, dy, timeMsec);
          }
        }
        m_middleAccum = 0;
        m_middlePressX = lx;
        m_middlePressY = ly;
        m_server->cursor()->overrideCursor("grabbing");
      }
      if (m_middlePanning) {
        if (m_middleScrolling) {
          m_server->gestures()->updatePointerScroll(lx - m_middlePressX, ly - m_middlePressY, timeMsec);
        }
        m_middlePressX = lx;
        m_middlePressY = ly;
        return;
      }
      m_middleAccum += horizontal ? lx - m_middlePressX : ly - m_middlePressY;
      m_middlePressX = lx;
      m_middlePressY = ly;
      while (m_middleAccum <= -kMiddleScrollStepPx) {
        m_middleAccum += kMiddleScrollStepPx;
        selectRelativeWorkspace(1, m_middleOutput.get());
      }
      while (m_middleAccum >= kMiddleScrollStepPx) {
        m_middleAccum -= kMiddleScrollStepPx;
        selectRelativeWorkspace(-1, m_middleOutput.get());
      }
      return;
    }
    if (m_dragCard != nullptr) {
      updateDrag(lx, ly);
      return;
    }
    if (m_pressThumb && m_pressWorkspace != nullptr) {
      if (!m_thumbDragging) {
        const double dx = lx - m_pressX;
        const double dy = ly - m_pressY;
        if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
          return;
        }
        m_thumbDragging = true;
        m_server->cursor()->overrideCursor("grabbing");
      }
      updateThumbDrag(lx, ly);
      return;
    }
    if (m_pressCard == nullptr) {
      updateHover(lx, ly);
      return;
    }
    const double dx = lx - m_pressX;
    const double dy = ly - m_pressY;
    if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
      return;
    }
    beginDrag();
    updateDrag(lx, ly);
  }

  bool Overview::selectRelativeWorkspace(int delta, Output* output) {
    if (!interactive()) {
      return false;
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return false;
    }
    WorkspaceGroup* group = output->workspaceGroup();
    if (group->active() == nullptr) {
      return false;
    }
    const int index = static_cast<int>(group->active()->index()) + delta;
    if (index < 0 || index >= static_cast<int>(group->workspaceCount())) {
      return false;
    }
    // select() lands in onWorkspaceActivated, which animates the filmstrip onto the new
    // workspace, so the caller does not have to arrange anything.
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  const View* Overview::closeButtonAt(double lx, double ly) const {
    for (const auto& state : m_outputs) {
      if (state->chrome != nullptr) {
        if (const void* key = state->chrome->closeAt(lx, ly)) {
          return static_cast<const View*>(key);
        }
      }
    }
    return nullptr;
  }

  bool Overview::menuPress(double lx, double ly) {
    if (m_menu.has_value()) {
      for (const auto& state : m_outputs) {
        if (state->output != m_menu->output || state->chrome == nullptr) {
          continue;
        }
        if (const int row = state->chrome->menuRowAt(lx, ly); row >= 0) {
          applyMenuRow(row);
          return true;
        }
        if (state->chrome->menuContains(lx, ly)) {
          return true;
        }
      }
      // A press outside the menu only closes it, the way a popup menu does.
      setMenu(std::nullopt);
      return true;
    }
    for (const auto& state : m_outputs) {
      if (state->chrome == nullptr) {
        continue;
      }
      if (const std::optional<size_t> workspace = state->chrome->chipAt(lx, ly)) {
        setMenu(ArrangementMenu{.output = state->output, .workspace = *workspace});
        return true;
      }
    }
    return false;
  }

  bool Overview::menuKey(uint32_t keysym) {
    if (!m_menu.has_value()) {
      return false;
    }
    ArrangementMenu menu = *m_menu;
    switch (keysym) {
    case XKB_KEY_Escape:
      setMenu(std::nullopt);
      break;
    case XKB_KEY_Up:
      menu.hovered = menu.hovered <= 0 ? ov::kMenuRows - 1 : menu.hovered - 1;
      setMenu(menu);
      break;
    case XKB_KEY_Down:
      menu.hovered = (menu.hovered + 1) % ov::kMenuRows;
      setMenu(menu);
      break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
      if (menu.hovered >= 0) {
        applyMenuRow(menu.hovered);
      }
      break;
    default:
      break; // the open menu holds the keyboard, so typing does not start a filter under it
    }
    return true;
  }

  void Overview::startRename() {
    Workspace* workspace = preferredWorkspace();
    if (workspace == nullptr || workspace->group() == nullptr) {
      return;
    }
    m_menu.reset();
    m_rename = Rename{.workspace = workspace, .text = workspace->displayName()};
    setMenu(std::nullopt);
  }

  bool Overview::renameKey(uint32_t keysym) {
    if (!m_rename.has_value()) {
      return false;
    }
    constexpr size_t kMaxNameBytes = 64;
    switch (keysym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      finishRename(true);
      return true;
    case XKB_KEY_Escape:
      finishRename(false);
      return true;
    case XKB_KEY_BackSpace:
      if (m_rename->fresh) {
        m_rename->text.clear();
      } else {
        ov::popFilterCharacter(m_rename->text);
      }
      break;
    default:
      if (std::string text = ov::filterText(keysym, false); !text.empty()) {
        if (m_rename->fresh) {
          m_rename->text.clear();
        }
        if (m_rename->text.size() + text.size() <= kMaxNameBytes) {
          m_rename->text += text;
        }
      }
      break; // the field holds the keyboard: other keys do nothing while it is open
    }
    m_rename->fresh = false;
    setMenu(std::nullopt);
    return true;
  }

  void Overview::finishRename(bool keep) {
    if (!m_rename.has_value()) {
      return;
    }
    Rename rename = std::move(*m_rename);
    m_rename.reset();
    const auto blank = std::ranges::all_of(rename.text, [](unsigned char c) { return std::isspace(c) != 0; });
    if (Workspace* workspace = rename.workspace.get(); keep && workspace != nullptr && !blank) {
      // Typing the configured name back clears the label.
      workspace->setLabel(rename.text == workspace->name() ? std::string() : std::move(rename.text));
    }
    setMenu(std::nullopt);
  }

  void Overview::setMenu(std::optional<ArrangementMenu> menu) {
    m_menu = menu;
    const View* liveTarget = liveTargetView();
    for (const auto& state : m_outputs) {
      layoutChrome(*state, liveTarget);
    }
  }

  void Overview::applyMenuRow(int row) {
    const ArrangementMenu menu = *m_menu;
    setMenu(std::nullopt);
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output == menu.output ? state->output->workspaceGroup() : nullptr;
      Workspace* workspace = group != nullptr ? group->workspaceAt(menu.workspace) : nullptr;
      const LayoutMode mode = kMenuModes[static_cast<size_t>(row)];
      if (workspace != nullptr && workspace->layoutMode() != mode) {
        workspace->overrideLayoutMode(mode);
        // The same swap as workspace-set-layout: a tiled resize on the old layout is stale.
        m_server->cursor()->cancelStaleTiledResize();
      }
    }
  }

  void Overview::openWindowMenu(const Card& card, double lx, double ly) {
    const Workspace* source = card.view->workspace();
    WorkspaceGroup* group = source != nullptr ? source->group() : nullptr;
    if (group == nullptr || group->output() == nullptr) {
      return;
    }
    WindowMenu menu{
        .output = group->output(),
        .view = card.view,
        .x = static_cast<int>(std::lround(lx)),
        .y = static_cast<int>(std::lround(ly)),
        .hovered = -1,
        .rows = {},
        .targets = {},
    };
    for (size_t index = 0; index < group->workspaceCount(); ++index) {
      Workspace* workspace = group->workspaceAt(index);
      if (workspace == nullptr || workspace == source) {
        continue;
      }
      const bool named = workspace->named() || workspace->displayName() != workspace->name();
      menu.rows.push_back(
          named ? std::format("Move to {}", workspace->displayName())
                : std::format("Move to Desktop {}", workspace->displayName())
      );
      menu.targets.emplace_back(workspace);
    }
    menu.rows.emplace_back("Move to New Desktop");
    menu.targets.emplace_back(nullptr);
    menu.rows.emplace_back("Close");
    menu.rows.emplace_back("Close All on This Desktop");
    setWindowMenu(std::move(menu));
  }

  void Overview::setWindowMenu(std::optional<WindowMenu> menu) {
    m_windowMenu = std::move(menu);
    const View* liveTarget = liveTargetView();
    for (const auto& state : m_outputs) {
      layoutChrome(*state, liveTarget);
    }
  }

  void Overview::applyWindowMenuRow(int row) {
    WindowMenu menu = std::move(*m_windowMenu);
    setWindowMenu(std::nullopt);
    View* view = menu.view.get();
    if (view == nullptr || !view->mapped() || row < 0) {
      return;
    }
    const auto index = static_cast<size_t>(row);
    if (index < menu.targets.size()) {
      moveViewTo(*view, menu.targets[index].get());
      return;
    }
    if (index == menu.targets.size()) {
      view->requestClose();
      return;
    }
    Workspace* workspace = view->workspace();
    const OutputState* state = workspace != nullptr ? stateForWorkspace(workspace) : nullptr;
    if (state != nullptr) {
      // Collected first: a close can unmap at once and change the card list.
      std::vector<Ref<View>> closing;
      for (const auto& card : state->cards) {
        if (card->view != nullptr && card->view->workspace() == workspace) {
          closing.emplace_back(card->view);
        }
      }
      for (const Ref<View>& each : closing) {
        if (each.get() != nullptr && each->mapped()) {
          each->requestClose();
        }
      }
    }
  }

  void Overview::moveViewTo(View& view, Workspace* target) {
    if (target == nullptr) {
      WorkspaceGroup* group = view.workspace() != nullptr ? view.workspace()->group() : nullptr;
      target = group != nullptr ? group->insertDynamicWorkspace(group->workspaceCount()) : nullptr;
    }
    if (target == nullptr || target == view.workspace()) {
      return;
    }
    if (view.floating()) {
      view.rememberFloatingPosition();
    }
    view.moveToWorkspace(target, true, LayoutAttachOrigin::MovedView);
    if (!view.tiled()) {
      target->exitFullscreenForIncomingView(&view);
    }
    if (view.floating()) {
      view.restoreFloatingPosition();
    }
  }

  bool Overview::windowMenuPress(double lx, double ly) {
    if (!m_windowMenu.has_value()) {
      return false;
    }
    for (const auto& state : m_outputs) {
      if (state->output != m_windowMenu->output || state->chrome == nullptr) {
        continue;
      }
      if (const int row = state->chrome->windowMenuRowAt(lx, ly); row >= 0) {
        applyWindowMenuRow(row);
        return true;
      }
      if (state->chrome->windowMenuContains(lx, ly)) {
        return true;
      }
    }
    // A press outside the menu only closes it, the way a popup menu does.
    setWindowMenu(std::nullopt);
    return true;
  }

  bool Overview::windowMenuKey(uint32_t keysym) {
    if (!m_windowMenu.has_value()) {
      return false;
    }
    const auto rows = static_cast<int>(m_windowMenu->rows.size());
    WindowMenu menu = *m_windowMenu;
    switch (keysym) {
    case XKB_KEY_Escape:
      setWindowMenu(std::nullopt);
      break;
    case XKB_KEY_Up:
      menu.hovered = menu.hovered <= 0 ? rows - 1 : menu.hovered - 1;
      setWindowMenu(std::move(menu));
      break;
    case XKB_KEY_Down:
      menu.hovered = (menu.hovered + 1) % rows;
      setWindowMenu(std::move(menu));
      break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      if (menu.hovered >= 0) {
        applyWindowMenuRow(menu.hovered);
      }
      break;
    default:
      break;
    }
    return true;
  }

  void Overview::updateHover(double lx, double ly) {
    if (m_windowMenu.has_value()) {
      int row = -1;
      for (const auto& state : m_outputs) {
        if (state->output == m_windowMenu->output && state->chrome != nullptr) {
          row = state->chrome->windowMenuRowAt(lx, ly);
        }
      }
      if (row != m_windowMenu->hovered) {
        WindowMenu menu = *m_windowMenu;
        menu.hovered = row;
        setWindowMenu(std::move(menu));
      }
    }
    if (m_menu.has_value()) {
      int row = -1;
      for (const auto& state : m_outputs) {
        if (state->output == m_menu->output && state->chrome != nullptr) {
          row = state->chrome->menuRowAt(lx, ly);
        }
      }
      if (row != m_menu->hovered) {
        ArrangementMenu menu = *m_menu;
        menu.hovered = row;
        setMenu(menu);
      }
    }
    const View* close = closeButtonAt(lx, ly);
    const View* hover = close;
    if (hover == nullptr) {
      const Card* card = cardAt(lx, ly);
      hover = card != nullptr ? card->view : nullptr;
    }
    const bool closeHover = close != nullptr;
    if (hover == m_hoverView && closeHover == m_closeHover) {
      return;
    }
    m_hoverView = hover;
    m_closeHover = closeHover;
    // Only the labels and the close button follow the pointer; the cards stay where they are.
    const View* liveTarget = liveTargetView();
    for (const auto& state : m_outputs) {
      layoutChrome(*state, liveTarget);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    cancelNavigation();
    if (!interactive()) {
      return true;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    const WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    const bool horizontalWorkspaces = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
    const int sign = direction < 0 ? -1 : 1;
    // Wheel input commits discrete targets on its physical axis, unlike continuous touchpad navigation.
    if (vertical != horizontalWorkspaces) {
      selectRelativeWorkspace(sign, output);
      return true;
    }
    Workspace* workspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true);
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return true;
    }
    View* target = vertical ? workspace->focusVertical(sign) : workspace->focusAdjacent(sign);
    if (target == nullptr) {
      return true;
    }
    if (workspace->active()) {
      m_server->focusView(target, FocusReason::Gesture);
    } else {
      workspace->setFocusedView(target);
    }
    workspace->activateFocusedColumn();
    scrolling->snapVisible(scrolling->columnOf(target), workspace->scrollViewportExtent());
    workspace->markArrange(true);
    return true;
  }

  Workspace* Overview::pointerScrollWorkspace(double lx, double ly) {
    if (!interactive()) {
      return nullptr;
    }
    return workspaceAtPoint(lx, ly, nullptr, nullptr, true);
  }

  Workspace* Overview::navigationWorkspace() const {
    const WorkspaceGroup* group = m_navigationOutput != nullptr ? m_navigationOutput->workspaceGroup() : nullptr;
    if (group != nullptr) {
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (group->workspaceAt(index) == m_navigationWorkspace) {
          return m_navigationWorkspace.get();
        }
      }
    }
    return nullptr;
  }

  void Overview::onNavigationDeviceDestroyed(Overview* self, void* /*data*/) { self->cancelNavigation(); }

  void Overview::cancelNavigation() { endNavigation(true, 0, m_navigationSource); }

  void Overview::beginNavigation(wlr_pointer* pointer, NavigationSource source, double lx, double ly) {
    cancelNavigation();
    if (!interactive() || dragging() || pointer == nullptr) {
      return;
    }
    Output* output = m_server->outputFromWlr(wlr_output_layout_output_at(m_server->outputLayout(), lx, ly));
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return;
    }
    m_navigationOutput = output;
    m_navigationHorizontalWorkspaces = output->workspaceGroup()->workspaceAxis() == WorkspaceAxis::Horizontal;
    m_navigationWorkspace = workspaceAtPoint(lx, ly, nullptr, nullptr, true);
    if (m_navigationWorkspace == nullptr || m_navigationWorkspace->group() != output->workspaceGroup()) {
      m_navigationWorkspace = output->workspaceGroup()->active();
    }
    m_navigationPointer = pointer;
    m_navigationDeviceDestroy.connect(&pointer->base.events.destroy, this, onNavigationDeviceDestroyed);
    m_navigationSource = source;
    m_navigation.reset();
    m_navigationStarted = false;
  }

  void Overview::updateNavigation(double dx, double dy, uint32_t timeMsec) {
    if (m_navigationOutput == nullptr || !interactive()) {
      return;
    }
    m_navigation.update(dx, dy, timeMsec);
    const auto axis = m_navigation.axis();
    if (axis == OverviewNavigation::Axis::Pending) {
      return;
    }
    OutputState* state = stateFor(m_navigationOutput);
    WorkspaceGroup* group = m_navigationOutput->workspaceGroup();
    if (state == nullptr || group == nullptr || group->active() == nullptr) {
      cancelNavigation();
      return;
    }
    const auto& overview = config().overview;
    const double factor =
        axis == OverviewNavigation::Axis::Horizontal ? overview.scrollFactorHorizontal : overview.scrollFactorVertical;
    const auto travel = OverviewNavigation::travelFor(m_navigationSource);
    if ((axis == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      if (!m_navigationStarted) {
        m_navigationStart = state->rowScroll.current();
        m_navigationScale =
            OverviewNavigation::travelScale(1.0, workspaceScale(group->active()), factor, travel.workspace);
        m_navigationStarted = true;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      // snap() also stops any settle still running on this output; row animations elsewhere and the zoom continue.
      state->rowScroll.snap(
          GesturePhysics::rubberBand(
              m_navigationStart + m_navigation.position() * m_navigationScale, 0.0, last,
              GesturePhysics::kOverscrollLimit
          )
      );
      applyProgress();
      return;
    }
    Workspace* workspace = navigationWorkspace();
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const auto viewport = static_cast<double>(workspace->scrollViewportExtent());
    if (!m_navigationStarted) {
      m_navigationStart = scrolling->scroll();
      m_navigationCentered = scrolling->centeredRest();
      m_navigationScale = OverviewNavigation::travelScale(viewport, workspaceScale(workspace), factor, travel.viewport);
      m_navigationStarted = true;
    }
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    // Centering an edge column parks the strip past the last column, so the band opens onto the scroll the gesture
    // started from: the fingers bring the strip back in, not the pan's first frame.
    scrolling->setScroll(
        GesturePhysics::rubberBand(
            m_navigationStart + m_navigation.position() * m_navigationScale, std::min(0.0, m_navigationStart),
            std::max(maximum, m_navigationStart), viewport * GesturePhysics::kOverscrollLimit
        )
    );
    workspace->markArrange(false);
  }

  void Overview::endNavigation(bool cancelled, uint32_t timeMsec, NavigationSource source) {
    if (source != m_navigationSource) {
      return;
    }
    Workspace* workspace = navigationWorkspace();
    Output* output = m_navigationOutput;
    // Clear the gesture before selecting or focusing below: both re-enter through onWorkspaceActivated and
    // onFocusChanged, which cancel the navigation belonging to this output.
    m_navigationOutput = nullptr;
    m_navigationWorkspace = nullptr;
    if (m_navigationPointer != nullptr) {
      m_navigationDeviceDestroy.disconnect();
      m_navigationPointer = nullptr;
    }
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    const bool started = m_navigationStarted;
    m_navigationStarted = false;
    if (!started || output == nullptr || !interactive()) {
      return;
    }
    if (!cancelled) {
      m_navigation.update(0, 0, timeMsec);
    }
    const double projected = m_navigationStart + m_navigation.projectedPosition() * m_navigationScale;
    if ((m_navigation.axis() == OverviewNavigation::Axis::Horizontal) == m_navigationHorizontalWorkspaces) {
      OutputState* state = stateFor(output);
      WorkspaceGroup* group = output->workspaceGroup();
      if (state == nullptr || group == nullptr || group->active() == nullptr) {
        return;
      }
      const auto last = static_cast<double>(group->workspaceCount() - 1);
      const int index = cancelled ? static_cast<int>(group->active()->index())
                                  : GesturePhysics::stepTarget(projected, 0, static_cast<int>(last));
      // Rubber-banded travel moves the filmstrip slower than the fingers, so the release carries the visible speed.
      const double position = m_navigationStart + m_navigation.position() * m_navigationScale;
      const double velocity = cancelled ? 0.0
                                        : m_navigation.velocity()
              * m_navigationScale
              * GesturePhysics::rubberBandDerivative(position, 0.0, last, GesturePhysics::kOverscrollLimit);
      if (index != static_cast<int>(group->active()->index())) {
        group->select(group->workspaceAt(static_cast<size_t>(index)));
        state = stateFor(output);
      }
      if (state != nullptr) {
        animateRow(*state, static_cast<double>(index), velocity);
      }
      return;
    }
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr) {
      return;
    }
    const int viewport = workspace->scrollViewportExtent();
    const auto maximum = static_cast<double>(scrolling->maxScroll(viewport));
    if (cancelled) {
      scrolling->setScroll(
          m_navigationCentered ? m_navigationStart : std::clamp(m_navigationStart, 0.0, maximum), m_navigationCentered
      );
      workspace->markArrange(true);
      return;
    }
    double bestDistance = std::numeric_limits<double>::max();
    double bestPosition = std::clamp(projected, 0.0, maximum);
    int bestColumn = -1;
    for (size_t index = 0; index < scrolling->columns().size(); ++index) {
      if (scrolling->columns()[index].views.empty()) {
        continue;
      }
      const auto column = static_cast<int>(index);
      for (const double position : scrolling->snapScrolls(column, viewport)) {
        const double distance = std::abs(position - projected);
        if (distance < bestDistance) {
          bestDistance = distance;
          bestPosition = position;
          bestColumn = column;
        }
      }
    }
    // Match normal strip navigation: focus the outermost fully visible column
    // in the travel direction, retaining the selected card within that column.
    const int direction = projected >= scrolling->scroll() ? 1 : -1;
    const auto count = static_cast<int>(scrolling->columns().size());
    for (int index = bestColumn + direction; bestColumn >= 0 && index >= 0 && index < count; index += direction) {
      const double x = scrolling->columnX(index, viewport);
      const double width = scrolling->columnWidth(index, viewport);
      if (x < bestPosition || x + width > bestPosition + viewport) {
        break;
      }
      bestColumn = index;
    }
    View* target = workspace->focusedView();
    const bool focusMoved = bestColumn >= 0 && (target == nullptr || scrolling->columnOf(target) != bestColumn);
    if (focusMoved) {
      // A tab group is entered on the tab it shows, as a directional focus move enters it.
      target = columnEntry(scrolling->columns()[static_cast<size_t>(bestColumn)]);
    }
    scrolling->setScroll(bestPosition);
    // Do not activate a different workspace merely because its preview was
    // panned. Its selected card will receive focus if the user enters that row.
    if (target != nullptr) {
      if (workspace->active()) {
        m_server->focusView(target, FocusReason::Gesture);
      } else {
        workspace->setFocusedView(target);
      }
    }
    // The pan chose the column, so the centering policy says where it rests. A pan back on the focused column has no
    // side to derive, so the last focus move's pair is judged instead.
    if (focusMoved) {
      workspace->activateFocusedColumn();
    } else {
      workspace->reevaluateFocusedColumn();
    }
    workspace->markArrange(true);
  }

  void Overview::handleTouchpadAxis(
      wlr_pointer* pointer, bool vertical, double delta, uint32_t timeMsec, double lx, double ly
  ) {
    // A three-finger swipe owns the overview until it ends; finger scrolling cannot arrive during one anyway.
    if (m_navigationPointer != nullptr && m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (pointer != m_navigationPointer) {
      if (delta == 0) {
        return;
      }
      beginNavigation(pointer, NavigationSource::Scroll, lx, ly);
    }
    if (m_navigationPointer == nullptr) {
      return;
    }
    if (vertical) {
      m_scrollDy += delta;
      m_scrollStopY = delta == 0;
    } else {
      m_scrollDx += delta;
      m_scrollStopX = delta == 0;
    }
    m_scrollTime = timeMsec;
  }

  void Overview::handleTouchpadFrame() {
    if (m_navigationPointer == nullptr || m_navigationSource != NavigationSource::Scroll) {
      return;
    }
    if (m_scrollDx != 0 || m_scrollDy != 0) {
      updateNavigation(m_scrollDx, m_scrollDy, m_scrollTime);
    }
    const auto axis = m_navigation.axis();
    const bool stop = axis == OverviewNavigation::Axis::Horizontal ? m_scrollStopX
        : axis == OverviewNavigation::Axis::Vertical               ? m_scrollStopY
                                                                   : m_scrollStopX || m_scrollStopY;
    m_scrollDx = m_scrollDy = 0;
    m_scrollStopX = m_scrollStopY = false;
    if (stop) {
      endNavigation(false, m_scrollTime, NavigationSource::Scroll);
    }
  }

  bool Overview::handleKeybindAction(KeybindAction action) {
    const bool directional = action == KeybindAction::WindowFocusLeft
        || action == KeybindAction::WindowFocusRight
        || action == KeybindAction::WindowFocusOrOutputLeft
        || action == KeybindAction::WindowFocusOrOutputRight
        || action == KeybindAction::WindowFocusUp
        || action == KeybindAction::WindowFocusDown
        || action == KeybindAction::WindowFocusOrWorkspaceUp
        || action == KeybindAction::WindowFocusOrWorkspaceDown
        || action == KeybindAction::WindowFocusOrOutputUp
        || action == KeybindAction::WindowFocusOrOutputDown;
    const bool outputFocus = action == KeybindAction::OutputFocusLeft
        || action == KeybindAction::OutputFocusRight
        || action == KeybindAction::OutputFocusUp
        || action == KeybindAction::OutputFocusDown;

    if (m_closing && (directional || outputFocus)) {
      m_pendingFocus = nullptr;
    }

    return false;
  }

  bool Overview::cardMatches(const Card& card) const {
    if (card.view == nullptr) {
      return false;
    }
    const char* title = card.view->title();
    const char* appId = card.view->appId();
    return ov::filterMatches(m_filter, title != nullptr ? title : "", appId != nullptr ? appId : "");
  }

  std::vector<Overview::Card*> Overview::selectableCards() {
    Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    struct Entry {
      Card* card;
      std::tuple<bool, size_t, bool, size_t, int, int> key;
    };
    std::vector<Entry> entries;
    for (size_t outputIndex = 0; outputIndex < m_outputs.size(); ++outputIndex) {
      const OutputState& state = *m_outputs[outputIndex];
      const WorkspaceGroup* group = state.output->workspaceGroup();
      const Workspace* active = group != nullptr ? group->active() : nullptr;
      for (size_t k = 0; k < state.cards.size(); ++k) {
        Card& card = *state.cards[k];
        const bool placed = k < state.placeOf.size() && state.placeOf[k] != nullptr;
        if (!placed || card.view == nullptr || !card.view->mapped() || card.opacity <= 0.0F || !cardMatches(card)) {
          continue;
        }
        const bool onActive = active != nullptr && card.view->workspace() == active;
        entries.push_back({
            .card = &card,
            .key = {
                state.output != preferred, outputIndex, !onActive, card.workspaceIndex,
                card.box.y + card.box.height / 2, card.box.x + card.box.width / 2
            },
        });
      }
    }
    std::ranges::stable_sort(entries, {}, &Entry::key);
    std::vector<Card*> cards;
    cards.reserve(entries.size());
    for (const Entry& entry : entries) {
      cards.push_back(entry.card);
    }
    return cards;
  }

  Overview::Card* Overview::cardOf(const View* view) {
    if (view == nullptr) {
      return nullptr;
    }
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Overview::Card* Overview::selectedCard() {
    Card* card = cardOf(liveTargetView());
    return card != nullptr && cardMatches(*card) ? card : nullptr;
  }

  void Overview::selectCard(Card& card) {
    View* view = card.view;
    Workspace* workspace = view != nullptr ? view->workspace() : nullptr;
    if (workspace == nullptr || workspace->group() == nullptr) {
      return;
    }
    if (workspace->group()->active() != workspace) {
      workspace->group()->select(workspace);
    }
    if (workspace->active()) {
      m_server->focusView(view, FocusReason::Gesture);
    } else {
      workspace->setFocusedView(view);
    }
    if (ScrollingLayout* scrolling = workspace->scrollingLayout()) {
      workspace->activateFocusedColumn();
      scrolling->snapVisible(scrolling->columnOf(view), workspace->scrollViewportExtent());
      workspace->markArrange(true);
    }
    applyProgress();
    scheduleFrames();
  }

  void Overview::setFilter(std::string filter) {
    if (filter == m_filter) {
      return;
    }
    m_filter = std::move(filter);
    if (!m_filter.empty() && selectedCard() == nullptr) {
      const std::vector<Card*> matches = selectableCards();
      if (!matches.empty()) {
        selectCard(*matches.front());
        return;
      }
    }
    applyProgress();
    scheduleFrames();
  }

  bool Overview::moveSelection(int dx, int dy) {
    const std::vector<Card*> cards = selectableCards();
    if (cards.empty()) {
      return true;
    }
    const Card* from = selectedCard();
    if (from == nullptr) {
      selectCard(*cards.front());
      return true;
    }
    const auto centre = [](const Card& card) {
      return ov::Point{card.box.x + card.box.width / 2.0, card.box.y + card.box.height / 2.0};
    };
    std::vector<ov::Point> centres;
    centres.reserve(cards.size());
    for (const Card* card : cards) {
      centres.push_back(card == from ? ov::Point{1e9, 1e9} : centre(*card));
    }
    const ov::Direction direction = dx < 0 ? ov::Direction::Left
        : dx > 0                           ? ov::Direction::Right
        : dy < 0                           ? ov::Direction::Up
                                           : ov::Direction::Down;
    const int pick = ov::pickInDirection(centres, centre(*from), direction);
    if (pick >= 0) {
      selectCard(*cards[static_cast<size_t>(pick)]);
    }
    return true;
  }

  bool Overview::cycleSelection(int step) {
    const std::vector<Card*> cards = selectableCards();
    if (cards.empty()) {
      return true;
    }
    const Card* current = selectedCard();
    int index = -1;
    for (size_t k = 0; k < cards.size(); ++k) {
      if (cards[k] == current) {
        index = static_cast<int>(k);
      }
    }
    selectCard(*cards[ov::cycleIndex(cards.size(), index, step)]);
    return true;
  }

  bool Overview::activateFilter() {
    Card* card = selectedCard();
    if (card == nullptr) {
      const std::vector<Card*> matches = selectableCards();
      card = matches.empty() ? nullptr : matches.front();
    }
    if (card != nullptr) {
      closeToWorkspace(card->view->workspace(), card->view);
      return true;
    }
    // Nothing open matches: hand the query to the launcher, which opens over the overview with it typed in.
    if (DesktopShell* shell = m_server->desktopShell()) {
      (void)shell->sendAction(std::format("panel-open launcher {}", m_filter));
    }
    return true;
  }

  bool Overview::handleFallbackKey(uint32_t keysym, bool shift) {
    if (!interactive()) {
      return false;
    }
    if (renameKey(keysym) || windowMenuKey(keysym) || menuKey(keysym)) {
      return true;
    }
    if (keysym == XKB_KEY_F2) {
      startRename();
      return true;
    }
    const bool back = keysym == XKB_KEY_Left || keysym == XKB_KEY_Up;
    if (shift && (back || keysym == XKB_KEY_Right || keysym == XKB_KEY_Down)) {
      Card* card = selectedCard();
      Workspace* source = card != nullptr && card->view != nullptr ? card->view->workspace() : nullptr;
      WorkspaceGroup* group = source != nullptr ? source->group() : nullptr;
      if (group != nullptr) {
        const size_t index = source->index();
        // Past the last workspace is a new one; before the first there is nothing.
        if (!back || index > 0) {
          View* view = card->view;
          moveViewTo(*view, back ? group->workspaceAt(index - 1)
                            : index + 1 < group->workspaceCount() ? group->workspaceAt(index + 1)
                                                                  : nullptr);
          // The overview follows the window and keeps it selected, so the next Shift+arrow moves it on.
          if (Card* moved = findCard(view)) {
            selectCard(*moved);
          }
        }
      }
      return true;
    }
    if (!m_filter.empty()) {
      switch (keysym) {
      case XKB_KEY_Escape:
        setFilter({});
        return true;
      case XKB_KEY_Return:
      case XKB_KEY_KP_Enter:
        return activateFilter();
      case XKB_KEY_Left:
        return moveSelection(-1, 0);
      case XKB_KEY_Right:
        return moveSelection(1, 0);
      case XKB_KEY_Up:
        return moveSelection(0, -1);
      case XKB_KEY_Down:
        return moveSelection(0, 1);
      default:
        break;
      }
    }
    switch (keysym) {
    case XKB_KEY_BackSpace: {
      std::string filter = m_filter;
      ov::popFilterCharacter(filter);
      setFilter(std::move(filter));
      return true;
    }
    case XKB_KEY_Tab:
      return cycleSelection(1);
    case XKB_KEY_ISO_Left_Tab:
      return cycleSelection(-1);
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
      if (Card* card = selectedCard()) {
        card->view->requestClose();
      }
      return true;
    default:
      break;
    }
    if (m_filter.empty()) {
      if (const int digit = ov::workspaceDigit(keysym); digit >= 0) {
        Output* output = m_server->outputFromWlr(m_server->preferredOutput());
        WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
        if (group != nullptr && static_cast<size_t>(digit) < group->workspaceCount()) {
          group->select(group->workspaceAt(static_cast<size_t>(digit)));
        }
        return true;
      }
    }
    if (std::string text = ov::filterText(keysym, m_filter.empty()); !text.empty()) {
      setFilter(m_filter + text);
      return true;
    }
    auto dispatch = [this](KeybindAction action) {
      Keybind bind;
      bind.action = action;
      return m_server->executeKeybindAction(bind);
    };
    // Local windows first, exactly like the composite focus actions. Only an arrow
    // pointing along the output's workspace axis falls through to a workspace step.
    const auto arrow = [this, &dispatch](int sign, bool horizontalArrow, KeybindAction action) {
      Workspace* workspace = preferredWorkspace();
      if (workspace == nullptr || workspace->group() == nullptr) {
        return true;
      }
      const bool alongWorkspaceAxis =
          (workspace->group()->workspaceAxis() == WorkspaceAxis::Horizontal) == horizontalArrow;
      const View* neighbor = horizontalArrow ? workspace->focusAdjacent(sign) : workspace->focusVertical(sign);
      if (neighbor != nullptr || !alongWorkspaceAxis) {
        return dispatch(action);
      }
      selectRelativeWorkspace(sign, workspace->group()->output());
      return true;
    };
    switch (keysym) {
    case XKB_KEY_Escape:
      close();
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      if (Workspace* workspace = preferredWorkspace()) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    case XKB_KEY_Left:
      return arrow(-1, true, KeybindAction::WindowFocusLeft);
    case XKB_KEY_Right:
      return arrow(1, true, KeybindAction::WindowFocusRight);
    case XKB_KEY_Up:
      return arrow(-1, false, KeybindAction::WindowFocusUp);
    case XKB_KEY_Down:
      return arrow(1, false, KeybindAction::WindowFocusDown);
    default:
      return false;
    }
  }

} // namespace andromeda
