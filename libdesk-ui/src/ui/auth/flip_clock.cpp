#include "ui/auth/flip_clock.h"

#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"
#include "ui/style.h"

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

void FlipClock::build(Node& parent, AnimationManager& animations) {
  m_animations = &animations;
  m_root = parent.addChild(std::make_unique<Node>());
  for (Card& card : m_cards) {
    card.group = m_root->addChild(std::make_unique<Node>());
    card.face = addRect(*card.group);
    card.shade = addRect(*card.group);
    auto label = std::make_unique<Label>();
    label->setFontWeight(FontWeight::SemiBold);
    label->setFontFeatures("tnum");
    card.text = static_cast<Label*>(card.group->addChild(std::move(label)));
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
    card.group->setPosition(x + static_cast<float>(i) * (w + std::round(6.0F * k)), y);
    card.group->setSize(w, h);
    card.face->setPosition(0.0F, 0.0F);
    card.face->setSize(w, h);
    card.face->setStyle(RoundedRectStyle{.fill = withAlpha(ink, 0.12F), .radius = radius});
    card.shade->setPosition(0.0F, std::round(h * 0.5F));
    card.shade->setSize(w, h - std::round(h * 0.5F));
    card.shade->setStyle(RoundedRectStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, 0.18F), .radius = Radii(0.0F, 0.0F, radius, radius)
    });
    card.text->setFontSize(40.0F * k);
    card.text->setColor(ink);
    const std::string& next = *texts[i];
    if (card.shown.empty() || card.shown == next || m_animations == nullptr) {
      if (!card.folding) {
        placeText(card, renderer, next);
      }
      continue;
    }
    if (card.folding) {
      continue;
    }
    // Fold shut on the card's centre line, swap the digits, open again.
    card.folding = true;
    Card* target = &card;
    Renderer* r = &renderer;
    m_animations->animateSpatial(
        1.0F, 0.0F, Style::spring(Tokens::springSnap), [target](float v) { target->group->setScale(1.0F, v); },
        [this, target, r, next] {
          placeText(*target, *r, next);
          m_animations->animateSpatial(
              0.0F, 1.0F, Style::spring(Tokens::springSnap), [target](float v) { target->group->setScale(1.0F, v); },
              [target] { target->folding = false; }, this
          );
        },
        this
    );
  }
}

void FlipClock::placeText(Card& card, Renderer& renderer, const std::string& text) {
  card.shown = text;
  card.text->setText(text);
  card.text->measure(renderer);
  card.text->setPosition(
      std::round((card.group->width() - card.text->width()) * 0.5F),
      std::round((card.group->height() - card.text->height()) * 0.5F)
  );
}
