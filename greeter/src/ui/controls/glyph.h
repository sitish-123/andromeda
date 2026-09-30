#pragma once

#include "render/scene/glyph_node.h"

#include <string>
#include <string_view>

class Glyph : public GlyphNode {
public:
  Glyph();

  void setGlyph(std::string_view name);
  void setGlyphSize(float size);
  void setColor(const Color& color);

private:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

  std::string m_glyphName;
};
