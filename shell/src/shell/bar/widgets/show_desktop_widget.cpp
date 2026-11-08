#include "shell/bar/widgets/show_desktop_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"

#include <memory>

namespace {
  // Board desktop-layouts.html section 1: an 8 px sliver with a full-length separator, fill_1 on hover.
  constexpr float kStrip = 8.0F;
} // namespace

ShowDesktopWidget::ShowDesktopWidget(bool verticalBar, Options /*options*/) : m_verticalBar(verticalBar) {}

void ShowDesktopWidget::create() {
  auto area = ui::inputArea({});
  auto hover = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .visible = false});
  Box* hoverBox = static_cast<Box*>(area->addChild(std::move(hover)));
  hoverBox->setParticipatesInLayout(false);
  m_hover = hoverBox;
  area->setOnEnter([this, hoverBox](const InputArea::PointerData&) {
    hoverBox->setVisible(true);
    requestRedraw();
  });
  area->setOnLeave([this, hoverBox] {
    hoverBox->setVisible(false);
    requestRedraw();
  });
  auto hairline = ui::box({.fill = colorSpecFromRole(ColorRole::Separator)});
  m_hairline = area->addChild(std::move(hairline));
  m_hairline->setParticipatesInLayout(false);
  setRoot(std::move(area));
}

void ShowDesktopWidget::doLayout(Renderer& /*renderer*/, float containerWidth, float containerHeight) {
  auto* node = root();
  if (node == nullptr || m_hairline == nullptr) {
    return;
  }
  const float strip = kStrip * m_contentScale;
  if (m_hover != nullptr) {
    m_hover->setPosition(0.0F, 0.0F);
    m_hover->setSize(m_verticalBar ? containerWidth : strip, m_verticalBar ? strip : containerHeight);
  }
  if (m_verticalBar) {
    node->setSize(containerWidth, strip);
    m_hairline->setPosition(0.0F, 0.0F);
    m_hairline->setSize(containerWidth, 1.0F);
  } else {
    node->setSize(strip, containerHeight);
    m_hairline->setPosition(0.0F, 0.0F);
    m_hairline->setSize(1.0F, containerHeight);
  }
}
