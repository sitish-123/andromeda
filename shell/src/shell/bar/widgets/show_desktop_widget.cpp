#include "shell/bar/widgets/show_desktop_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <memory>

namespace {
  constexpr float kStrip = 8.0F;
  constexpr float kHairlineShare = 0.6F;
} // namespace

ShowDesktopWidget::ShowDesktopWidget(bool verticalBar, Options /*options*/) : m_verticalBar(verticalBar) {}

void ShowDesktopWidget::create() {
  auto area = ui::inputArea({});
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
  if (m_verticalBar) {
    node->setSize(containerWidth, strip);
    m_hairline->setPosition(containerWidth * (1.0F - kHairlineShare) * 0.5F, 0.0F);
    m_hairline->setSize(containerWidth * kHairlineShare, 1.0F);
  } else {
    node->setSize(strip, containerHeight);
    m_hairline->setPosition(0.0F, containerHeight * (1.0F - kHairlineShare) * 0.5F);
    m_hairline->setSize(1.0F, containerHeight * kHairlineShare);
  }
}
