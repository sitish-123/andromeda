#include "input/cursor.h"
#include "input/cursor_internal.h"

#include "core/log.h"
#include "input/input_capture.h"
#include "input/seat.h"
#include "overview/overview.h"
#include "server/dsk_surface.h"
#include "view/view.h"
// clang-format off
#include <linux/input-event-codes.h>
// clang-format on


namespace andromeda {

  using namespace cursor_detail;

  namespace {
    constexpr Logger kLog("cursor");
  } // namespace

  void Cursor::onTouchDown(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchDown);
    self->handleTouchDown(data);
  }

  void Cursor::onTouchUp(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchUp);
    self->handleTouchUp(data);
  }

  void Cursor::onTouchMotion(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchMotion);
    self->handleTouchMotion(data);
  }

  void Cursor::onTouchCancel(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchCancel);
    self->handleTouchCancel(data);
  }

  void Cursor::onTouchFrame(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchFrame);
    self->handleTouchFrame();
  }

  void Cursor::handleTouchDown(void* data) {
    if (InputCapture* capture = m_server->inputCapture(); capture != nullptr) {
      capture->suspend();
    }
    auto* event = static_cast<wlr_touch_down_event*>(data);
    m_server->notifyInputActivity();
    m_server->cancelModifierTap();
    m_server->remapTouches();

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(lx, ly, &surface, &sx, &sy, &layer);

    // A tap in overview activates like a left click; panels keep their touch.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked() && !overviewPassthroughLayer(layer)) {
      if (overview->interactive()) {
        overview->handleButton(BTN_LEFT, true, lx, ly, event->time_msec);
        overview->handleButton(BTN_LEFT, false, lx, ly, event->time_msec);
      }
      return;
    }

    if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
      // A tap on a tab bar selects that tab, as a left click does.
      if (const std::optional<TabHit> hit = tabBarHitAt(*m_server, lx, ly, surface, view, layer)) {
        if (hit->part != TabBarPart::Tab) {
          Workspace* home = hit->tab->workspace();
          if (View* target = home != nullptr
                  ? home->tabs().cycleFrom(hit->tab, hit->part == TabBarPart::PreviousTab ? -1 : 1)
                  : nullptr) {
            m_server->focusView(target, FocusReason::PointerPress);
          }
        } else {
          m_server->focusView(hit->tab, FocusReason::PointerPress);
        }
        return;
      }
    }

    if (surface != nullptr && !m_trackerTouch && !m_server->sessionLocked() && m_server->dskSurfaces() != nullptr) {
      if (const std::optional<bool> horizontal = m_server->dskSurfaces()->dragAxisHorizontal(surface, sx, sy)) {
        m_trackerTouch = TrackerTouch{
            .touchId = event->touch_id,
            .drag = {.surface = surface, .sx = sx, .sy = sy, .startX = lx, .startY = ly, .horizontal = *horizontal},
            .lastX = lx,
            .lastY = ly,
        };
      }
    }

    if (surface != nullptr) {
      // Focus the touched view (click-to-focus equivalent).
      if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
        if (layer != nullptr) {
          if (!isXdgPopupSurface(surface)) {
            layer->focus();
          }
        } else if (view != nullptr && !isXdgPopupSurface(surface)) {
          m_server->focusView(view, FocusReason::PointerPress);
        }
      }
      wlr_seat_touch_notify_down(m_server->seat()->wlr(), surface, event->time_msec, event->touch_id, sx, sy);
    }
  }

  void Cursor::handleTouchUp(void* data) {
    auto* event = static_cast<wlr_touch_up_event*>(data);
    m_server->notifyIdleActivity();
    if (m_trackerTouch && m_trackerTouch->touchId == event->touch_id) {
      const bool claimed = m_trackerTouch->drag.claimed;
      m_trackerTouch.reset();
      if (claimed) {
        // The client already got a cancel for this point.
        m_server->dskSurfaces()->endSwipe(event->time_msec);
        return;
      }
    }
    wlr_seat_touch_notify_up(m_server->seat()->wlr(), event->time_msec, event->touch_id);
  }

  void Cursor::handleTouchMotion(void* data) {
    auto* event = static_cast<wlr_touch_motion_event*>(data);
    m_server->notifyInputActivity();

    wlr_seat* seat = m_server->seat()->wlr();
    if (m_trackerTouch && m_trackerTouch->touchId == event->touch_id) {
      double tx = 0;
      double ty = 0;
      wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &tx, &ty);
      if (handleTrackerTouchMotion(event->time_msec, tx, ty)) {
        return;
      }
    }
    wlr_touch_point* point = wlr_seat_touch_get_point(seat, event->touch_id);
    if (point == nullptr) {
      kLog.warn("touch motion id={} has no active seat point", event->touch_id);
      return;
    }
    if (point->surface == nullptr) {
      kLog.warn("touch motion id={} has no target surface", event->touch_id);
      return;
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    if (!surfaceLocalCoordinates(m_server->scene(), point->surface, surfaceScale(point->surface), lx, ly, &sx, &sy)) {
      kLog.warn(
          "touch motion id={} could not map target surface {} at layout=({}, {})", event->touch_id,
          static_cast<void*>(point->surface), lx, ly
      );
      return;
    }
    wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id, sx, sy);
  }

  void Cursor::handleTouchCancel(void* data) {
    auto* event = static_cast<wlr_touch_cancel_event*>(data);
    (void)event;
    m_server->notifyIdleActivity();
    if (m_trackerTouch && m_trackerTouch->drag.claimed) {
      m_server->dskSurfaces()->cancelSwipe();
    }
    m_trackerTouch.reset();

    wlr_seat* seat = m_server->seat()->wlr();
    // Find the first client with an active touch point, then cancel outside
    // the iteration: wlr_seat_touch_notify_cancel may mutate the list.
    wlr_seat_client* client = nullptr;
    wlr_touch_point* point;
    wl_list_for_each(point, &seat->touch_state.touch_points, link) {
      if (point->client != nullptr) {
        client = point->client;
        break;
      }
    }
    if (client != nullptr) {
      wlr_seat_touch_notify_cancel(seat, client);
    }
  }

  void Cursor::handleTouchFrame() { wlr_seat_touch_notify_frame(m_server->seat()->wlr()); }

  bool Cursor::handleTrackerTouchMotion(uint32_t timeMsec, double lx, double ly) {
    DskSurfaceManager* dsk = m_server->dskSurfaces();
    TrackerTouch& touch = *m_trackerTouch;
    if (touch.drag.claimed) {
      dsk->updateSwipe(lx - touch.lastX, ly - touch.lastY, timeMsec);
    } else {
      const double dx = lx - touch.drag.startX;
      const double dy = ly - touch.drag.startY;
      if (std::hypot(dx, dy) < kTrackerDragSlop) {
        return false;
      }
      const bool alongAxis = touch.drag.horizontal ? std::abs(dx) > std::abs(dy) : std::abs(dy) > std::abs(dx);
      if (m_server->sessionLocked() || !alongAxis
          || !dsk->beginSwipe(touch.drag.surface, touch.drag.sx, touch.drag.sy, 0, timeMsec)) {
        m_trackerTouch.reset();
        return false;
      }
      touch.drag.claimed = true;
      wlr_seat* seat = m_server->seat()->wlr();
      if (wlr_touch_point* point = wlr_seat_touch_get_point(seat, touch.touchId); point != nullptr && point->client) {
        wlr_seat_touch_notify_cancel(seat, point->client);
      }
      dsk->updateSwipe(dx, dy, timeMsec);
    }
    touch.lastX = lx;
    touch.lastY = ly;
    for (const auto& output : m_server->outputs()) {
      wlr_output_schedule_frame(output->wlr());
    }
    return true;
  }

  bool Cursor::processTrackerDragMotion(uint32_t timeMsec, double oldX, double oldY) {
    if (!m_trackerDrag) {
      return false;
    }
    DskSurfaceManager* dsk = m_server->dskSurfaces();
    if (m_server->sessionLocked()) {
      if (m_trackerDrag->claimed) {
        dsk->cancelSwipe();
      }
      m_trackerDrag.reset();
      return false;
    }
    if (m_trackerDrag->claimed) {
      dsk->updateSwipe(m_cursor->x - oldX, m_cursor->y - oldY, timeMsec);
      for (const auto& output : m_server->outputs()) {
        wlr_output_schedule_frame(output->wlr());
      }
      return true;
    }
    const double dx = m_cursor->x - m_trackerDrag->startX;
    const double dy = m_cursor->y - m_trackerDrag->startY;
    if (std::hypot(dx, dy) < kTrackerDragSlop) {
      return false;
    }
    // A drag across the tracker's axis stays the client's (a slider, a text selection).
    const bool alongAxis = m_trackerDrag->horizontal ? std::abs(dx) > std::abs(dy) : std::abs(dy) > std::abs(dx);
    if (!alongAxis || !dsk->beginSwipe(m_trackerDrag->surface, m_trackerDrag->sx, m_trackerDrag->sy, 0, timeMsec)) {
      m_trackerDrag.reset();
      return false;
    }
    m_trackerDrag->claimed = true;
    // The leave ends the client's press: toolkits drop a pressed state when the pointer leaves.
    wlr_seat_pointer_notify_clear_focus(m_server->seat()->wlr());
    dsk->updateSwipe(dx, dy, timeMsec);
    for (const auto& output : m_server->outputs()) {
      wlr_output_schedule_frame(output->wlr());
    }
    return true;
  }

} // namespace andromeda
