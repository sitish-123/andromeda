#include "input/cursor.h"

#include "input/event_time.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
// clang-format off
// clang-format on
#include "workspace/workspace.h"


namespace andromeda {

  namespace {
    constexpr int kDataDragEdgeScrollTickMs = 16;
    constexpr uint32_t kDataDragEdgeScrollMaxElapsedMs = 50;
  } // namespace

  Workspace* Cursor::dataDragEdgeScrollTarget(double* speed) const {
    *speed = 0;
    const Config::Input::DragEdgeScroll& edge = config().input.dragEdgeScroll;
    if (!edge.enabled
        || m_server->sessionLocked()
        || (m_server->seat()->wlr()->drag == nullptr && !tiledMoveDragActive())
        || (m_server->overview() != nullptr && m_server->overview()->active())) {
      return nullptr;
    }

    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
    Workspace* workspace = group != nullptr ? group->active() : nullptr;
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr || scrolling->columns().empty()) {
      return nullptr;
    }

    const wlr_box area = workspace->usableArea();
    const bool vertical = workspace->scrollingVertical();
    const double origin = vertical ? area.y : area.x;
    const double extent = vertical ? area.height : area.width;
    if (extent <= 0) {
      return nullptr;
    }
    const double trigger = std::min(static_cast<double>(edge.triggerZone), extent / 2.0);
    const double position = vertical ? m_cursor->y : m_cursor->x;
    if (position < origin + trigger) {
      *speed = -edge.maxSpeed * std::clamp((origin + trigger - position) / trigger, 0.0, 1.0);
    } else if (position > origin + extent - trigger) {
      *speed = edge.maxSpeed * std::clamp((position - (origin + extent - trigger)) / trigger, 0.0, 1.0);
    }
    if (*speed == 0) {
      return nullptr;
    }

    const double oldScroll = scrolling->scroll();
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    if (maximum <= 0) {
      return nullptr;
    }
    if ((*speed < 0 && oldScroll <= 0) || (*speed > 0 && oldScroll >= maximum)) {
      return nullptr;
    }
    return workspace;
  }

  void Cursor::updateDataDragEdgeScroll() {
    const Config::Input::DragEdgeScroll& edge = config().input.dragEdgeScroll;
    double speed = 0;
    Workspace* workspace = dataDragEdgeScrollTarget(&speed);
    const int direction = (speed > 0) - (speed < 0);
    const int previousDirection = (m_dataDragEdgeScrollSpeed > 0) - (m_dataDragEdgeScrollSpeed < 0);
    if (workspace == nullptr) {
      cancelDataDragEdgeScroll();
      return;
    }
    if (workspace == m_dataDragEdgeScrollWorkspace && direction == previousDirection) {
      m_dataDragEdgeScrollSpeed = speed;
      return;
    }
    if (m_dataDragEdgeScrollTimer == nullptr) {
      m_dataDragEdgeScrollTimer =
          wl_event_loop_add_timer(wl_display_get_event_loop(m_server->display()), onDataDragEdgeScrollTimer, this);
      if (m_dataDragEdgeScrollTimer == nullptr) {
        return;
      }
    }
    m_dataDragEdgeScrollWorkspace = workspace;
    m_dataDragEdgeScrollSpeed = speed;
    m_dataDragEdgeScrollLastMsec = 0;
    m_dataDragEdgeScrollPending = true;
    if (edge.delayMs > 0) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, edge.delayMs);
    } else {
      handleDataDragEdgeScrollTimer();
    }
  }

  void Cursor::cancelDataDragEdgeScroll() {
    if (m_dataDragEdgeScrollTimer != nullptr) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, 0);
    }
    m_dataDragEdgeScrollWorkspace = nullptr;
    m_dataDragEdgeScrollSpeed = 0;
    m_dataDragEdgeScrollLastMsec = 0;
    m_dataDragEdgeScrollPending = false;
  }

  int Cursor::onDataDragEdgeScrollTimer(void* data) {
    return static_cast<Cursor*>(data)->handleDataDragEdgeScrollTimer();
  }

  int Cursor::handleDataDragEdgeScrollTimer() {
    double speed = 0;
    Workspace* workspace = dataDragEdgeScrollTarget(&speed);
    const int direction = (speed > 0) - (speed < 0);
    const int activeDirection = (m_dataDragEdgeScrollSpeed > 0) - (m_dataDragEdgeScrollSpeed < 0);
    if (workspace == nullptr) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    if (workspace != m_dataDragEdgeScrollWorkspace || direction != activeDirection) {
      updateDataDragEdgeScroll();
      return 0;
    }
    m_dataDragEdgeScrollSpeed = speed;

    const uint32_t now = monotonicMsec();
    if (m_dataDragEdgeScrollPending) {
      m_dataDragEdgeScrollPending = false;
      m_dataDragEdgeScrollLastMsec = now;
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
      return 0;
    }

    const uint32_t elapsed = std::min(now - m_dataDragEdgeScrollLastMsec, kDataDragEdgeScrollMaxElapsedMs);
    m_dataDragEdgeScrollLastMsec = now;
    if (elapsed == 0) {
      wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
      return 0;
    }
    ScrollingLayout* scrolling = workspace->scrollingLayout();
    if (scrolling == nullptr) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    const auto maximum = static_cast<double>(scrolling->maxScroll(workspace->scrollViewportExtent()));
    const double oldScroll = scrolling->scroll();
    const double nextScroll = std::clamp(oldScroll + speed * static_cast<double>(elapsed) / 1000.0, 0.0, maximum);
    if (nextScroll == oldScroll) {
      cancelDataDragEdgeScroll();
      return 0;
    }
    scrolling->setScroll(nextScroll);
    workspace->markArrange(false);
    wl_event_source_timer_update(m_dataDragEdgeScrollTimer, kDataDragEdgeScrollTickMs);
    return 0;
  }

} // namespace andromeda
