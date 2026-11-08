#include "shell/bar/widgets/launcher_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

LauncherWidget::LauncherWidget(wl_output* /*output*/, Options options)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_iconSize(options.iconSize), m_cellSize(options.cellSize) {}

void LauncherWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)),
        })
    );
  }

  setRoot(std::move(area));
}

void LauncherWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }

  if (m_image != nullptr) {
    // The image draws at baseGlyphSize x scale: icon_size rescales it.
    const float scale = m_iconSize > 0 ? m_contentScale * static_cast<float>(m_iconSize) / Style::baseGlyphSize
                                       : m_contentScale;
    widget_custom_image::sync(*m_image, renderer, m_customImage, scale, widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
    fitCell(*node, *m_image);
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize((m_iconSize > 0 ? static_cast<float>(m_iconSize) : Style::baseGlyphSize) * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::Label1)));
    m_glyph->measure(renderer);
    fitCell(*node, *m_glyph);
  }
}

void LauncherWidget::fitCell(Node& node, Node& icon) const {
  const float cell = static_cast<float>(m_cellSize) * m_contentScale;
  const float w = std::max(cell, icon.width());
  const float h = std::max(cell, icon.height());
  icon.setPosition(std::round((w - icon.width()) * 0.5F), std::round((h - icon.height()) * 0.5F));
  node.setSize(w, h);
}
