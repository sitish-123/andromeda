#pragma once

#include "render/core/color.h"
#include "render/core/damage_region.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"

#include <optional>
#include <string>

class TextNode : public Node {
public:
  TextNode() : Node(NodeType::Text) {}

  [[nodiscard]] const std::string& text() const noexcept { return m_text; }
  [[nodiscard]] float fontSize() const noexcept { return m_fontSize; }
  [[nodiscard]] const Color& color() const noexcept { return m_color; }
  [[nodiscard]] float maxWidth() const noexcept { return m_maxWidth; }
  [[nodiscard]] int maxLines() const noexcept { return m_maxLines; }
  [[nodiscard]] const TextFont& font() const noexcept { return m_font; }
  [[nodiscard]] FontWeight fontWeight() const noexcept { return m_font.weight; }
  [[nodiscard]] const std::string& fontFamily() const noexcept { return m_font.family; }
  [[nodiscard]] const std::string& fontFeatures() const noexcept { return m_font.features; }

  // Area the text paints, relative to the node origin (its baseline); empty until the owner measures it.
  [[nodiscard]] const DamageRect& paintExtent() const noexcept { return m_paintExtent; }
  void setPaintExtent(const DamageRect& extent) noexcept { m_paintExtent = extent; }

  void setText(std::string text) {
    if (m_text == text) {
      return;
    }
    m_text = std::move(text);
    markContentDirty();
  }

  void setFontSize(float size) {
    if (m_fontSize == size) {
      return;
    }
    m_fontSize = size;
    m_font.tracking = m_trackingOverride.value_or(dynamicTracking(size));
    markContentDirty();
  }

  // Glyph outlines only, `em` wide; 0 = filled glyphs.
  void setStroke(float em) {
    if (m_font.stroke == em) {
      return;
    }
    m_font.stroke = em;
    markContentDirty();
  }

  // Diagonal ink gradient, packed 0xRRGGBBAA; both 0 = the label color.
  void setGradient(std::uint32_t from, std::uint32_t to) {
    if (m_font.gradientFrom == from && m_font.gradientTo == to) {
      return;
    }
    m_font.gradientFrom = from;
    m_font.gradientTo = to;
    markContentDirty();
  }

  // Extra font axes ("wdth=75"); empty = the font's defaults.
  void setVariations(std::string variations) {
    if (m_font.variations == variations) {
      return;
    }
    m_font.variations = std::move(variations);
    markContentDirty();
  }

  // Letter spacing in em in place of the Inter dynamic tracking; nullopt restores it.
  void setTracking(std::optional<float> em) {
    if (m_trackingOverride == em) {
      return;
    }
    m_trackingOverride = em;
    m_font.tracking = em.value_or(dynamicTracking(m_fontSize));
    markContentDirty();
  }

  void setColor(const Color& color) {
    if (m_color == color) {
      return;
    }
    m_color = color;
    markPaintDirty();
  }

  void setMaxWidth(float maxWidth) {
    if (m_maxWidth == maxWidth) {
      return;
    }
    m_maxWidth = maxWidth;
    markContentDirty();
  }

  void setMaxLines(int maxLines) {
    if (m_maxLines == maxLines) {
      return;
    }
    m_maxLines = maxLines;
    markContentDirty();
  }

  void setFontWeight(FontWeight fontWeight) {
    if (m_font.weight == fontWeight) {
      return;
    }
    m_font.weight = fontWeight;
    markContentDirty();
  }

  void setFontFamily(std::string family) {
    if (m_font.family == family) {
      return;
    }
    m_font.family = std::move(family);
    markContentDirty();
  }

  void setFontFeatures(std::string features) {
    if (m_font.features == features) {
      return;
    }
    m_font.features = std::move(features);
    markContentDirty();
  }

  [[nodiscard]] TextAlign textAlign() const noexcept { return m_textAlign; }

  void setTextAlign(TextAlign align) {
    if (m_textAlign == align) {
      return;
    }
    m_textAlign = align;
    markContentDirty();
  }

  [[nodiscard]] TextEllipsize ellipsize() const noexcept { return m_ellipsize; }

  void setEllipsize(TextEllipsize ellipsize) {
    if (m_ellipsize == ellipsize) {
      return;
    }
    m_ellipsize = ellipsize;
    markContentDirty();
  }

  [[nodiscard]] bool useMarkup() const noexcept { return m_useMarkup; }

  void setUseMarkup(bool markup) {
    if (m_useMarkup == markup) {
      return;
    }
    m_useMarkup = markup;
    markContentDirty();
  }

  [[nodiscard]] bool hasShadow() const noexcept { return m_hasShadow; }
  [[nodiscard]] const Color& shadowColor() const noexcept { return m_shadowColor; }
  [[nodiscard]] float shadowOffsetX() const noexcept { return m_shadowOffsetX; }
  [[nodiscard]] float shadowOffsetY() const noexcept { return m_shadowOffsetY; }

  void setShadow(const Color& color, float offsetX, float offsetY) {
    m_hasShadow = true;
    m_shadowColor = color;
    m_shadowOffsetX = offsetX;
    m_shadowOffsetY = offsetY;
    markPaintDirty();
  }

  void clearShadow() {
    if (!m_hasShadow) {
      return;
    }
    m_hasShadow = false;
    markPaintDirty();
  }

private:
  // New text can keep the same bounds, so damage tracking needs a self-paint mark as well as a layout pass.
  void markContentDirty() {
    markLayoutDirty();
    markPaintDirty();
  }

  std::string m_text;
  TextFont m_font;
  float m_fontSize = 14.0F;
  std::optional<float> m_trackingOverride;
  float m_maxWidth = 0.0F;
  int m_maxLines = 0;
  Color m_color;
  TextAlign m_textAlign = TextAlign::Start;
  TextEllipsize m_ellipsize = TextEllipsize::End;
  bool m_useMarkup = false;
  bool m_hasShadow = false;
  Color m_shadowColor;
  float m_shadowOffsetX = 0.0F;
  float m_shadowOffsetY = 0.0F;
  DamageRect m_paintExtent;
};
