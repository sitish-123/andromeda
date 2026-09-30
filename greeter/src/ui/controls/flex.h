#pragma once

#include "render/core/color.h"
#include "render/scene/node.h"

#include <cstdint>
#include <memory>

class RectNode;

enum class FlexDirection { Horizontal, Vertical };
enum class FlexAlign { Start, Center, End, Stretch };

class Flex : public Node {
public:
  Flex();

  void setDirection(FlexDirection direction);
  void setAlign(FlexAlign align);
  void setGap(float gap);
  void setPadding(float padding);
  void setPadding(float horizontal, float vertical);
  void setPadding(float left, float top, float right, float bottom);
  void setRadius(float radius);
  void setFill(const Color& fill);
  void setBorder(const Color& border, float width);
  void setMinWidth(float width);
  void setMinHeight(float height);

  [[nodiscard]] float paddingLeft() const noexcept { return m_paddingLeft; }
  [[nodiscard]] float paddingRight() const noexcept { return m_paddingRight; }
  [[nodiscard]] float paddingTop() const noexcept { return m_paddingTop; }
  [[nodiscard]] float paddingBottom() const noexcept { return m_paddingBottom; }

protected:
  void ensureBackground();
  void syncBackgroundStyle();

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

  RectNode* m_background = nullptr;
  FlexDirection m_direction = FlexDirection::Horizontal;
  FlexAlign m_align = FlexAlign::Start;
  float m_gap = 0.0f;
  float m_paddingLeft = 0.0f;
  float m_paddingRight = 0.0f;
  float m_paddingTop = 0.0f;
  float m_paddingBottom = 0.0f;
  float m_radius = 0.0f;
  Color m_fill;
  Color m_border;
  float m_borderWidth = 0.0f;
  float m_minWidth = 0.0f;
  float m_minHeight = 0.0f;
};
