#pragma once

#include "render/core/color.h"

class Node;
class RectNode;

// Rulebook 9.15 analog clock style (minimal dial): a translucent disc, hour and minute hands, a center dot.
class AnalogClock {
public:
  void build(Node& parent);
  void setVisible(bool visible);
  // Lays the dial out at (x, y) with `diameter`; the hands show hour (0-23) and minute (0-59).
  void layout(float x, float y, float diameter, int hour, int minute, Color ink);

private:
  Node* m_root = nullptr;
  RectNode* m_dial = nullptr;
  Node* m_hourPivot = nullptr;
  RectNode* m_hourHand = nullptr;
  Node* m_minutePivot = nullptr;
  RectNode* m_minuteHand = nullptr;
  RectNode* m_dot = nullptr;
};
