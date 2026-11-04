#pragma once

#include "render/core/color.h"

#include <array>
#include <string>

class AnimationManager;
class Label;
class Node;
class RectNode;
class Renderer;

// Rulebook 9.15 flip clock style: hours and minutes on two cards, the lower half of each card shaded. A card whose
// digits change folds shut and opens on the new digits.
class FlipClock {
public:
  void build(Node& parent, AnimationManager& animations);
  void setVisible(bool visible);
  // Cards scale with `clockSize` (a 64 px clock = 60 x 72 cards); returns the total width.
  [[nodiscard]] static float widthFor(float clockSize);
  void layout(Renderer& renderer, float x, float y, float clockSize, const std::string& hours, const std::string& minutes,
              Color ink);

private:
  struct Card {
    Node* group = nullptr;
    RectNode* face = nullptr;
    RectNode* shade = nullptr;
    Label* text = nullptr;
    std::string shown;
    bool folding = false;
  };
  void placeText(Card& card, Renderer& renderer, const std::string& text);

  AnimationManager* m_animations = nullptr;
  Node* m_root = nullptr;
  std::array<Card, 2> m_cards{};
};
