#include "ui/auth/flip_clock.h"

#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"

#include <cmath>
#include <memory>

namespace {

  // At a 64 px clock: cards 60 x 72 (the mockup's 52 is narrower than Inter "18" at 44), radius 10, gap 6, digits 40 SemiBold, card white 12 %, lower half
  // black 18 %.
  constexpr float kMockClock = 64.0F;

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

} // namespace

void FlipClock::build(Node& parent) {
  m_root = parent.addChild(std::make_unique<Node>());
  for (Card& card : m_cards) {
    card.face = addRect(*m_root);
    card.shade = addRect(*m_root);
    auto label = std::make_unique<Label>();
    label->setFontWeight(FontWeight::SemiBold);
    label->setFontFeatures("tnum");
    card.text = static_cast<Label*>(m_root->addChild(std::move(label)));
  }
  m_root->setVisible(false);
}

void FlipClock::setVisible(bool visible) { m_root->setVisible(visible); }

float FlipClock::widthFor(float clockSize) {
  const float k = clockSize / kMockClock;
  return std::round((60.0F * 2.0F + 6.0F) * k);
}

void FlipClock::layout(
    Renderer& renderer, float x, float y, float clockSize, const std::string& hours, const std::string& minutes,
    Color ink
) {
  const float k = clockSize / kMockClock;
  const float w = std::round(60.0F * k);
  const float h = std::round(72.0F * k);
  const float radius = 10.0F * k;
  const std::array<const std::string*, 2> texts{&hours, &minutes};
  for (std::size_t i = 0; i < m_cards.size(); ++i) {
    Card& card = m_cards[i];
    const float cx = x + static_cast<float>(i) * (w + std::round(6.0F * k));
    card.face->setPosition(cx, y);
    card.face->setSize(w, h);
    card.face->setStyle(RoundedRectStyle{.fill = withAlpha(ink, 0.12F), .radius = radius});
    card.shade->setPosition(cx, y + std::round(h * 0.5F));
    card.shade->setSize(w, h - std::round(h * 0.5F));
    card.shade->setStyle(RoundedRectStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, 0.18F), .radius = Radii(0.0F, 0.0F, radius, radius)
    });
    card.text->setFontSize(40.0F * k);
    card.text->setColor(ink);
    card.text->setText(*texts[i]);
    card.text->measure(renderer);
    card.text->setPosition(
        std::round(cx + (w - card.text->width()) * 0.5F), std::round(y + (h - card.text->height()) * 0.5F)
    );
  }
}
