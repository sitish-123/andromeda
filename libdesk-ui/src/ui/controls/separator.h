#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>

class RectNode;

enum class SeparatorOrientation : std::uint8_t {
  // Infer from parent: horizontal rule inside a vertical Flex, vertical rule inside a horizontal Flex.
  Auto,
  HorizontalRule,
  VerticalRule,
};

class Separator : public Node {
public:
  Separator();

  void setColor(const ColorSpec& color);
  void setThickness(float thickness);
  void setOrientation(SeparatorOrientation orientation);
  void setGradientEdges(bool enabled);
  // Draws a horizontal rule this far past its end edge (to a card edge outside the parent padding, R6).
  void setEndOutset(float outset);
  // Empty space added on both sides of the rule, along the rule's cross axis
  // (top+bottom for a horizontal rule, left+right for a vertical one).
  void setSpacing(float spacing);

protected:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

private:
  [[nodiscard]] bool ruleIsHorizontal() const;
  void applyPalette();

  RectNode* m_rectStart = nullptr;
  RectNode* m_rectEnd = nullptr;
  ColorSpec m_color = colorSpecFromRole(ColorRole::Stroke);
  float m_thickness = 1.0F;
  float m_spacing = 0.0F;
  float m_endOutset = 0.0F;
  SeparatorOrientation m_orientation = SeparatorOrientation::Auto;
  bool m_gradientEdges = true;
  Signal<>::ScopedConnection m_paletteConn;
};
