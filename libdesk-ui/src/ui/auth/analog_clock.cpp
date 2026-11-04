#include "ui/auth/analog_clock.h"

#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"

#include <memory>
#include <numbers>

namespace {

  // Mockup dial: 104 px, disc white 8 % with a white 50 % edge; hour hand 28 x 4, minute hand 38 x 2.5, dot 6.
  constexpr float kMockDial = 104.0F;

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  // A pivot is a square centered on the dial: nodes rotate around their center, so the hand (its upper half) turns
  // around the dial center.
  void layoutHand(Node* pivot, RectNode* hand, float cx, float cy, float length, float width, float angle, Color ink) {
    pivot->setPosition(cx - length, cy - length);
    pivot->setSize(length * 2.0F, length * 2.0F);
    pivot->setRotation(angle);
    hand->setPosition(length - width * 0.5F, 0.0F);
    hand->setSize(width, length + width * 0.5F);
    hand->setStyle(RoundedRectStyle{.fill = ink, .radius = width * 0.5F});
  }

} // namespace

void AnalogClock::build(Node& parent) {
  m_root = parent.addChild(std::make_unique<Node>());
  m_dial = addRect(*m_root);
  m_hourPivot = m_root->addChild(std::make_unique<Node>());
  m_hourHand = addRect(*m_hourPivot);
  m_minutePivot = m_root->addChild(std::make_unique<Node>());
  m_minuteHand = addRect(*m_minutePivot);
  m_dot = addRect(*m_root);
  m_root->setVisible(false);
}

void AnalogClock::setVisible(bool visible) { m_root->setVisible(visible); }

void AnalogClock::layout(float x, float y, float diameter, int hour, int minute, Color ink) {
  const float k = diameter / kMockDial;
  const float r = diameter * 0.5F;
  m_dial->setPosition(x, y);
  m_dial->setSize(diameter, diameter);
  m_dial->setStyle(RoundedRectStyle{
      .fill = withAlpha(ink, 0.08F), .border = withAlpha(ink, 0.5F), .radius = r, .borderWidth = 1.0F * k
  });
  const float cx = x + r;
  const float cy = y + r;
  constexpr float kTurn = 2.0F * std::numbers::pi_v<float>;
  const float minuteAngle = kTurn * static_cast<float>(minute) / 60.0F;
  const float hourAngle = kTurn * (static_cast<float>(hour % 12) + static_cast<float>(minute) / 60.0F) / 12.0F;
  layoutHand(m_hourPivot, m_hourHand, cx, cy, 28.0F * k, 4.0F * k, hourAngle, ink);
  layoutHand(m_minutePivot, m_minuteHand, cx, cy, 38.0F * k, 2.5F * k, minuteAngle, ink);
  const float dot = 6.0F * k;
  m_dot->setPosition(cx - dot * 0.5F, cy - dot * 0.5F);
  m_dot->setSize(dot, dot);
  m_dot->setStyle(RoundedRectStyle{.fill = ink, .radius = dot * 0.5F});
}
