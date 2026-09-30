#pragma once

#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "render/scene/text_node.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Label : public InputArea {
public:
  Label();

  bool setText(std::string_view text);
  void setFontSize(float size);
  void setFontFamily(std::string family);
  void setColor(const Color& color);
  void setBold(bool bold);
  void setMinWidth(float minWidth);
  void setMaxWidth(float maxWidth);
  void setMaxLines(int maxLines);
  void setTextAlign(TextAlign align);

  [[nodiscard]] const std::string& text() const noexcept { return m_plainText; }
  [[nodiscard]] float fontSize() const noexcept;
  [[nodiscard]] const Color& color() const noexcept;
  [[nodiscard]] bool bold() const noexcept;
  [[nodiscard]] TextAlign textAlign() const noexcept;

  void measure(Renderer& renderer);
  LayoutSize measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints);

private:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

  TextNode* m_textNode = nullptr;
  std::string m_plainText;
  Color m_color;
  float m_minWidth = 0.0f;
  float m_userMaxWidth = 0.0f;
  int m_userMaxLines = 0;
  bool m_measureCached = false;
  std::string m_cachedText;
  float m_cachedFontSize = 0;
  bool m_cachedBold = false;
  float m_cachedMaxWidth = 0;
  int m_cachedMaxLines = 0;
};
