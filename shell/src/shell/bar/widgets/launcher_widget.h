#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class Glyph;
class Image;
struct wl_output;

class LauncherWidget : public Widget {
public:
  struct Options {
    std::string glyph = "search";
    std::string customImage;
    bool customImageColorize = false;
    // Logical px; 0 = the bar glyph size. The Windows taskbar Start button is 24 (board section 1).
    int iconSize = 0;
  };

  LauncherWidget(wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  int m_iconSize = 0;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
};
