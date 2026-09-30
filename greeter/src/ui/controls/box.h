#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"

class RectNode;

class Box : public Node {
public:
  Box();

  void setStyle(const RoundedRectStyle& style);
  [[nodiscard]] const RoundedRectStyle& style() const noexcept { return m_style; }

  void setSize(float w, float h);
  void setFrameSize(float w, float h);

private:
  void syncStyle();

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

  RectNode* m_rect = nullptr;
  RoundedRectStyle m_style;
};
