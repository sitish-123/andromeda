#include "input/cursor.h"

#include "input/seat.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
// clang-format on
#include "workspace/workspace.h"


namespace andromeda {

  namespace {
    constexpr double kHotCornerExtent = 8.0;
  } // namespace

  const Keybind* Cursor::hotCornerAction(size_t* cornerIndex) const {
    const Config::HotCorners& configured = config().hotCorners;
    if (!isPassthrough() || m_server->sessionLocked() || m_server->exclusiveKeyboardLayer() != nullptr) {
      return nullptr;
    }
    const wlr_seat* seat = m_server->seat()->wlr();
    if (seat->drag != nullptr || seat->pointer_state.button_count != 0) {
      return nullptr;
    }

    wlr_output* output = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    wlr_box box{};
    if (output == nullptr) {
      return nullptr;
    }
    wlr_output_layout_get_box(m_server->outputLayout(), output, &box);

    // A small logical area lets delayed corners remain reachable beside another output.
    const double horizontalExtent = std::min(kHotCornerExtent, box.width / 2.0);
    const double verticalExtent = std::min(kHotCornerExtent, box.height / 2.0);
    const bool left = m_cursor->x < box.x + horizontalExtent;
    const bool right = m_cursor->x >= box.x + box.width - horizontalExtent;
    const bool top = m_cursor->y < box.y + verticalExtent;
    const bool bottom = m_cursor->y >= box.y + box.height - verticalExtent;
    size_t index = configured.corners.size();
    if (left && top) {
      index = 0;
    } else if (right && top) {
      index = 1;
    } else if (left && bottom) {
      index = 2;
    } else if (right && bottom) {
      index = 3;
    }
    if (index == configured.corners.size()) {
      return nullptr;
    }
    const Config::HotCorner& corner = configured.corners[index];
    if (!corner.enabled || !corner.action) {
      return nullptr;
    }
    const Output* andromedaOutput = m_server->outputFromWlr(output);
    View* focused = View::fromSurface(seat->keyboard_state.focused_surface);
    // Corner handling precedes pointer-output refocus. Use the destination's remembered focus when the seat is
    // elsewhere, including click-to-focus sessions where crossing outputs does not move keyboard focus at all.
    if (andromedaOutput != nullptr && (focused == nullptr || focused->currentOutput() != andromedaOutput)) {
      const WorkspaceGroup* group = andromedaOutput->workspaceGroup();
      const Workspace* workspace = group != nullptr ? group->active() : nullptr;
      focused = workspace != nullptr ? workspace->focusedView() : nullptr;
    }
    if (focused != nullptr
        && focused->mapped()
        && focused->onActiveWorkspace()
        && focused->currentOutput() == andromedaOutput
        && (focused->layoutFullscreen() || focused->currentFullscreen())) {
      return nullptr;
    }
    if (cornerIndex != nullptr) {
      *cornerIndex = index;
    }
    return &*corner.action;
  }

  void Cursor::cancelHotCorner() {
    m_hotCornerPending = false;
    m_hotCornerTriggered = false;
    m_hotCornerIndex = config().hotCorners.corners.size();
    if (m_hotCornerTimer != nullptr) {
      wl_event_source_timer_update(m_hotCornerTimer, 0);
    }
  }

  void Cursor::updateHotCorner() {
    size_t cornerIndex = 0;
    const Keybind* action = hotCornerAction(&cornerIndex);
    if (action == nullptr) {
      cancelHotCorner();
      return;
    }
    if (cornerIndex != m_hotCornerIndex) {
      cancelHotCorner();
      m_hotCornerIndex = cornerIndex;
    }
    updatePointerOutput();
    if (m_hotCornerTriggered) {
      return;
    }
    const int delayMs = config().hotCorners.corners[cornerIndex].delayMs;
    if (delayMs == 0) {
      m_hotCornerTriggered = true;
      Keybind triggered = *action;
      m_server->executeKeybindAction(triggered);
      return;
    }
    if (m_hotCornerPending) {
      return;
    }
    if (m_hotCornerTimer == nullptr) {
      m_hotCornerTimer =
          wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onHotCornerTimer, this);
      if (m_hotCornerTimer == nullptr) {
        return;
      }
    }
    m_hotCornerPending = true;
    wl_event_source_timer_update(m_hotCornerTimer, delayMs);
  }

  int Cursor::onHotCornerTimer(void* data) {
    auto* cursor = static_cast<Cursor*>(data);
    cursor->m_hotCornerPending = false;
    size_t cornerIndex = 0;
    if (const Keybind* action = cursor->hotCornerAction(&cornerIndex);
        action != nullptr && cornerIndex == cursor->m_hotCornerIndex) {
      cursor->m_hotCornerTriggered = true;
      Keybind triggered = *action;
      cursor->m_server->executeKeybindAction(triggered);
    }
    return 0;
  }

} // namespace andromeda
