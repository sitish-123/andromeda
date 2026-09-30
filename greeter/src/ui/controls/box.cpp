#include "ui/controls/box.h"

#include "render/scene/rect_node.h"

#include <memory>

Box::Box() {
  auto rect = std::make_unique<RectNode>();
  m_rect = static_cast<RectNode*>(addChild(std::move(rect)));
  m_style = m_rect->style();
}

void Box::setStyle(const RoundedRectStyle& style) {
  m_style = style;
  syncStyle();
}

void Box::setSize(float w, float h) {
  Node::setSize(w, h);
  if (m_rect != nullptr) {
    m_rect->setFrameSize(w, h);
  }
}

void Box::setFrameSize(float w, float h) {
  Node::setFrameSize(w, h);
  if (m_rect != nullptr) {
    m_rect->setFrameSize(w, h);
  }
}

void Box::syncStyle() {
  if (m_rect != nullptr) {
    m_rect->setStyle(m_style);
  }
}

LayoutSize Box::doMeasure(Renderer&, const LayoutConstraints& constraints) {
  float w = width() > 0 ? width() : 0;
  float h = height() > 0 ? height() : 0;
  return constraints.constrain({w, h});
}

void Box::doLayout(Renderer& renderer) {
  if (m_rect != nullptr) {
    m_rect->setPosition(0.0f, 0.0f);
    m_rect->setSize(width(), height());
  }
  for (auto& child : m_children) {
    if (child.get() != m_rect && child->visible()) {
      child->layout(renderer);
    }
  }
}
