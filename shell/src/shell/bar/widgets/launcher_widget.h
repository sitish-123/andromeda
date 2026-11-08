#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class Glyph;
class Image;
class Node;
struct wl_output;

class LauncherWidget : public Widget {
public:
  struct Options {
    std::string glyph = "search";
    std::string customImage;
    bool customImageColorize = false;
    // Logical px; 0 = the bar glyph size.
    int iconSize = 0;
    // Logical px square the icon centres in; 0 = the icon's own size. The Start button matches the taskbar cells.
    int cellSize = 0;
  };

  LauncherWidget(wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void fitCell(Node& node, Node& icon) const;
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  int m_iconSize = 0;
  int m_cellSize = 0;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
};
