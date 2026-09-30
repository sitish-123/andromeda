#include "ui/controls/glyph.h"

#include "render/text/glyph_registry.h"

#include <algorithm>

Glyph::Glyph() = default;

void Glyph::setGlyph(std::string_view name) {
  m_glyphName = std::string(name);
  setCodepoint(GlyphRegistry::lookup(name));
}

void Glyph::setGlyphSize(float size) { setFontSize(std::max(1.0f, size)); }
void Glyph::setColor(const Color& color) { GlyphNode::setColor(color); }

LayoutSize Glyph::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  if (codepoint() == 0) {
    return constraints.constrain({0, 0});
  }
  const auto metrics = renderer.measureGlyph(codepoint(), fontSize());
  return constraints.constrain({metrics.width, metrics.bottom - metrics.top});
}

void Glyph::doLayout(Renderer&) {}
