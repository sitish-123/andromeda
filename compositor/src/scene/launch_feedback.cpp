#include "scene/launch_feedback.h"

#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace andromeda {

  namespace {
    constexpr int kTravelMs = 420;
    constexpr int kExitMs = 220;
    constexpr double kTravelScale = 1.5;   // the icon at the target, relative to its size in the dock or launcher
    constexpr double kBreathAmount = 0.05; // scale swing while the app starts
    constexpr double kBreathPeriodMs = 1100.0;
    constexpr double kExitGrowth = 1.2; // extra scale when the window replaces the icon
  } // namespace

  LaunchFeedback::LaunchFeedback(
      Server& server, Output* output, wlr_scene_tree* parent, wlr_buffer* buffer, const wlr_fbox& source,
      const wlr_box& from, const wlr_box& area
  )
      : m_server(&server), m_output(output), m_buffer(wlr_buffer_lock(buffer)), m_from(from),
        m_targetX(area.x + area.width / 2.0), m_targetY(area.y + area.height / 2.0) {
    m_tree = wlr_scene_tree_create(parent);
    m_node = wlr_scene_buffer_create(m_tree, m_buffer);
    wlr_scene_buffer_set_source_box(m_node, &source);
    m_alpha.snap(1.0);
    m_travel.retarget(1.0, kTravelMs, Easing::EaseOutCubic);
    apply(0.0);
    m_server->registerAnimatable(this);
    if (m_output != nullptr) {
      m_output->scheduleFullFrame();
    }
  }

  LaunchFeedback::~LaunchFeedback() {
    m_server->unregisterAnimatable(this);
    wlr_scene_node_destroy(&m_tree->node);
    wlr_buffer_unlock(m_buffer);
  }

  void LaunchFeedback::finish() {
    if (m_finishing) {
      return;
    }
    m_finishing = true;
    m_exit.retarget(1.0, kExitMs, Easing::EaseOutCubic);
    if (m_output != nullptr) {
      m_output->scheduleFullFrame();
    }
  }

  bool LaunchFeedback::finished() const { return m_finishing && !m_exit.animating(); }

  bool LaunchFeedback::tickAnimations(double nowMsec) {
    m_travel.tick(nowMsec);
    m_exit.tick(nowMsec);
    apply(nowMsec);
    return hasActiveAnimations();
  }

  void LaunchFeedback::apply(double nowMsec) {
    const double travel = m_travel.current();
    double scale = 1.0 + (kTravelScale - 1.0) * travel;
    if (!m_travel.animating() && nowMsec > 0.0) {
      if (m_breathStartMsec < 0.0) {
        m_breathStartMsec = nowMsec;
      }
      const double phase = (nowMsec - m_breathStartMsec) / kBreathPeriodMs * 2.0 * std::numbers::pi;
      scale *= 1.0 + kBreathAmount * std::sin(phase);
    }
    const double exit = m_exit.current();
    scale *= 1.0 + kExitGrowth * exit;

    const double fromX = m_from.x + m_from.width / 2.0;
    const double fromY = m_from.y + m_from.height / 2.0;
    const double centreX = fromX + (m_targetX - fromX) * travel;
    const double centreY = fromY + (m_targetY - fromY) * travel;
    const int width = std::max(1, static_cast<int>(std::lround(m_from.width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(m_from.height * scale)));
    int parentX = 0;
    int parentY = 0;
    wlr_scene_node_coords(&m_tree->node.parent->node, &parentX, &parentY);
    wlr_scene_node_set_position(
        &m_tree->node, static_cast<int>(std::lround(centreX - width / 2.0)) - parentX,
        static_cast<int>(std::lround(centreY - height / 2.0)) - parentY
    );
    wlr_scene_buffer_set_dest_size(m_node, width, height);
    wlr_scene_buffer_set_opacity(m_node, static_cast<float>(m_alpha.current() * (1.0 - exit)));
  }

} // namespace andromeda
