#include "input/cursor.h"
#include "input/cursor_internal.h"

#include "input/input_capture.h"
#include "input/seat.h"
// clang-format off
#include <linux/input-event-codes.h>
// clang-format on


namespace andromeda {

  using namespace cursor_detail;

  void Cursor::setToolEmulating(TabletToolState* state, bool emulating) {
    if (state->emulating == emulating) {
      return;
    }
    if (emulating) {
      // Native → emulating: end the client's stroke cleanly.
      if (state->v2->focused_surface != nullptr) {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(state->v2);
      }
    } else {
      // Emulating → native: the surface must never receive doubled pointer and
      // tablet input for the same stroke.
      clearPointerFocusOverridingGrab();
    }
    state->emulating = emulating;
  }

  void Cursor::handleTabletToolAxis(void* data) {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      capture->suspend();
    }
    auto* event = static_cast<wlr_tablet_tool_axis_event*>(data);
    noteActivity();
    m_server->notifyInputActivity();
    TabletToolState* state = toolState(event->tool);
    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    m_server->remapTablets();
    if (event->tool->type == WLR_TABLET_TOOL_TYPE_MOUSE || event->tool->type == WLR_TABLET_TOOL_TYPE_LENS) {
      wlr_cursor_move(m_cursor, &event->tablet->base, event->dx, event->dy);
    } else {
      if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_X) != 0) {
        state->x = event->x;
      }
      if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) != 0) {
        state->y = event->y;
      }
      wlr_cursor_warp_absolute(m_cursor, &event->tablet->base, state->x, state->y);
    }
    processTabletMotion(event->time_msec, oldX, oldY, state, event->tablet);
    if (state->emulating) {
      return;
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) != 0) {
      wlr_tablet_v2_tablet_tool_notify_pressure(state->v2, event->pressure);
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) != 0) {
      state->tiltX = event->tilt_x;
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) != 0) {
      state->tiltY = event->tilt_y;
    }
    if ((event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) != 0) {
      wlr_tablet_v2_tablet_tool_notify_tilt(state->v2, state->tiltX, state->tiltY);
    }
    if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) != 0) {
      wlr_tablet_v2_tablet_tool_notify_distance(state->v2, event->distance);
    }
  }

  void Cursor::handleTabletToolProximity(void* data) {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      capture->suspend();
    }
    auto* event = static_cast<wlr_tablet_tool_proximity_event*>(data);
    noteActivity();
    if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
      TabletToolState* state = toolState(event->tool);
      state->inProximity = true;
      state->x = event->x;
      state->y = event->y;
      const double oldX = m_cursor->x;
      const double oldY = m_cursor->y;
      m_server->remapTablets();
      wlr_cursor_warp_absolute(m_cursor, &event->tablet->base, event->x, event->y);
      processTabletMotion(event->time_msec, oldX, oldY, state, event->tablet);
      return;
    }
    // Proximity out: look up without creating; an unknown tool never produced events.
    TabletToolState* state = nullptr;
    for (const auto& entry : m_tools) {
      if (entry->tool == event->tool) {
        state = entry.get();
        break;
      }
    }
    if (state == nullptr) {
      return;
    }
    // Release a tip that was left down through emulation.
    if (state->emulating && state->tipDown) {
      processButton(event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
    }
    if (!state->emulating && state->v2->focused_surface != nullptr) {
      wlr_tablet_v2_tablet_tool_notify_proximity_out(state->v2);
    }
    state->tipDown = false;
    state->inProximity = false;
  }

  void Cursor::handleTabletToolTip(void* data) {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      capture->suspend();
    }
    auto* event = static_cast<wlr_tablet_tool_tip_event*>(data);
    noteActivity();
    if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    if (event->state == WLR_TABLET_TOOL_TIP_DOWN) {
      m_server->cancelModifierTap();
    }
    TabletToolState* state = toolState(event->tool);
    const bool down = event->state == WLR_TABLET_TOOL_TIP_DOWN;
    if (state->emulating) {
      state->tipDown = down;
      processButton(
          event->time_msec, BTN_LEFT, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
      );
      wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
      return;
    }
    if (down) {
      state->tipDown = true;
      // Click-to-focus parity with processButton.
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (layer != nullptr) {
        if (!isXdgPopupSurface(surface)) {
          layer->focus();
        }
      } else if (m_server->exclusiveKeyboardLayer() == nullptr && view != nullptr && !isXdgPopupSurface(surface)) {
        m_server->focusView(view, FocusReason::PointerPress);
      }
      wlr_tablet_v2_tablet_tool_notify_down(state->v2);
      wlr_tablet_tool_v2_start_implicit_grab(state->v2);
    } else {
      state->tipDown = false;
      wlr_tablet_v2_tablet_tool_notify_up(state->v2);
    }
  }

  void Cursor::handleTabletToolButton(void* data) {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      capture->suspend();
    }
    auto* event = static_cast<wlr_tablet_tool_button_event*>(data);
    noteActivity();
    if (event->state == WLR_BUTTON_PRESSED) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    TabletToolState* state = toolState(event->tool);
    const bool pressed = event->state == WLR_BUTTON_PRESSED;
    if (!state->emulating) {
      wlr_tablet_v2_tablet_tool_notify_button(
          state->v2, event->button,
          pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED
      );
      return;
    }
    uint32_t mapped = event->button;
    if (mapped == BTN_STYLUS) {
      mapped = BTN_RIGHT;
    } else if (mapped == BTN_STYLUS2) {
      mapped = BTN_MIDDLE;
    } else if (mapped == BTN_STYLUS3) {
      mapped = BTN_SIDE;
    }
    processButton(
        event->time_msec, mapped, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    wlr_seat_pointer_notify_frame(m_server->seat()->wlr());
  }

} // namespace andromeda
