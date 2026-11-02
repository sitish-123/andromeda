#include "shell/control_center/tabs/quick_tab.h"

#include "dbus/link/link_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/panel/panel_manager.h"
#include "system/brightness_service.h"
#include "system/rfkill_helper.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "config/config_service.h"
#include "ui/controls/slider.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <utility>

namespace {

  // Anatomy: docs/design-system.md "Control center"; values: protocol/design-tokens.toml [component.*].
  TOKENS_LIVE(kRoundIcon, Tokens::componentRoundIconSize);
  TOKENS_LIVE(kPillHeight, Tokens::componentSliderHeight);
  constexpr float kChevronGlyph = 12.0F;
  constexpr float kChevronHit = 12.0F; // the glyph sits at inset 12; the hit area grows by 6 each side

  // The slider fills the whole pill; the icon and the value label draw over it.
  class SliderPill : public Flex {
  public:
    Slider* slider = nullptr;
    std::function<void()> restyle; // symbol colours depend on the arranged width

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      if (slider != nullptr) {
        slider->arrange(renderer, LayoutRect{.width = width(), .height = height()});
      }
      if (restyle) {
        restyle();
      }
    }
  };

  // A tile with an edit badge: a remove badge at -4 / -4, or an add badge centred at the right inset.
  class BadgedCard : public Flex {
  public:
    Flex* badge = nullptr;
    bool addMode = false;
    float scale = 1.0F;

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      if (badge == nullptr) {
        return;
      }
      const float size = (addMode ? Tokens::componentEditBadgeAdd : Tokens::componentEditBadgeRemove) * scale;
      const float x = addMode ? width() - Tokens::componentTilePadding * scale - size
                              : -Tokens::componentEditBadgeInset * scale;
      const float y = addMode ? (height() - size) * 0.5F : -Tokens::componentEditBadgeInset * scale;
      badge->arrange(renderer, LayoutRect{.x = x, .y = y, .width = size, .height = size});
    }
  };

  // A Button's own hit area has z-index 1 and covers its children; nested buttons must sit above it.
  constexpr std::int32_t kAboveCardArea = 2;

  std::string percent(float value) { return std::format("{}%", static_cast<int>(std::lround(value * 100.0F))); }

  std::unique_ptr<Label> makeLabel(Label*& out, const Tokens::Type& type, float scale, ColorRole role) {
    auto label = std::make_unique<Label>();
    label->setFontSize(type.size * scale);
    label->setLineHeight(type.line * scale);
    label->setFontWeight(static_cast<FontWeight>(Style::glassWeight(type.weight)));
    label->setColor(colorSpecFromRole(role));
    label->setMaxLines(1);
    out = label.get();
    return label;
  }

  std::unique_ptr<Button> makeGlyphButton(
      Button*& out, std::string_view glyph, float scale, std::function<void()> click, float size = kRoundIcon,
      ButtonVariant variant = ButtonVariant::Tonal
  ) {
    auto button = std::make_unique<Button>();
    button->setVariant(variant);
    button->setGlyph(glyph);
    button->setGlyphSize(Tokens::componentRoundIconGlyph * scale);
    button->setControlHeight(size * scale);
    button->setMinWidth(size * scale);
    button->setPadding(0.0F);
    button->setRadius(size * 0.5F * scale);
    button->setOnClick(std::move(click));
    out = button.get();
    return button;
  }

  // Card hover and press (docs: DESIGN.md "Quick-sheet row hover"): the card goes fill-1 -> fill-2 and scales 0.97.
  struct CardMotion {
    float hover = 0.0F;
    float press = 0.0F;
    AnimationManager::Id hoverId = 0;
    AnimationManager::Id pressId = 0;
  };

  desk::motion::SpringConfig springOf(const Tokens::Spring& token) {
    return {.damping = token.damping, .stiffness = token.stiffness};
  }

  // The row owns the animations (Node cancels them when the row dies); the state lives as long as a setter holds it.
  void driveCard(
      Button& row, const std::shared_ptr<CardMotion>& motion, float CardMotion::* value,
      AnimationManager::Id CardMotion::* idMember, float target, const Tokens::Spring& token,
      std::function<void(Flex&, float)> apply
  ) {
    AnimationManager::Id& id = motion.get()->*idMember;
    auto* card = dynamic_cast<Flex*>(row.parent());
    AnimationManager* manager = row.animationManager();
    if (card == nullptr || manager == nullptr) {
      return;
    }
    if (id != 0 && manager->retarget(id, target, springOf(token))) {
      return;
    }
    id = manager->animateSpring(
        motion.get()->*value, target, springOf(token),
        [card, motion, value, apply = std::move(apply)](float v) {
          motion.get()->*value = v;
          apply(*card, v);
        },
        {}, &row
    );
  }

  void attachCardMotion(Button& row, float opacity, Button* icon) {
    row.setStateFillless(true);
    auto motion = std::make_shared<CardMotion>();
    Button* rowPtr = &row;
    const auto hoverTo = [rowPtr, motion, opacity, icon](float target) {
      driveCard(*rowPtr, motion, &CardMotion::hover, &CardMotion::hoverId, target, Tokens::springFade, [opacity, icon](Flex& card, float t) {
        icon->setHoverMix(t);
        card.setFill(
            lerpColor(
                resolveColorSpec(colorSpecFromRole(ColorRole::Fill1, opacity)),
                resolveColorSpec(colorSpecFromRole(ColorRole::Fill2, opacity)), t
            )
        );
      });
    };
    row.setOnEnter([hoverTo] { hoverTo(1.0F); });
    row.setOnLeave([hoverTo] { hoverTo(0.0F); });
    row.setOnPress([rowPtr, motion](float, float, bool pressed) {
      driveCard(
          *rowPtr, motion, &CardMotion::press, &CardMotion::pressId, pressed ? 1.0F : 0.0F,
          pressed ? Tokens::springPress : Tokens::springRelease,
          [](Flex& card, float t) {
            card.setTransformOrigin(card.width() * 0.5F, card.height() * 0.5F);
            card.setScale(1.0F - 0.03F * t);
          }
      );
    });
  }

} // namespace

QuickTab::QuickTab(const ControlCenterServices& services, OpenTabCallback openTab)
    : m_services(services), m_openTab(std::move(openTab)) {
  m_phoneRing = ShortcutRegistry::create("phone", services.shortcutServices());
}

std::unique_ptr<Flex> QuickTab::makeCard(bool badged) {
  const float s = contentScale();
  std::unique_ptr<Flex> card = badged ? std::make_unique<BadgedCard>() : std::make_unique<Flex>();
  control_center::applySectionCardStyle(*card, s, panelCardOpacity());
  // Cards on a material are fill-1 (docs/design-system.md), lighter than the sheet; the shared card style is fill-2.
  card->setFill(colorSpecFromRole(ColorRole::Fill1, panelCardOpacity()));
  card->clearBorder();
  card->setRadius(Tokens::componentTileRadius * s);
  card->setPadding(Tokens::componentTilePadding * s);
  card->setGap(Tokens::componentTileGap * s);
  card->setJustify(FlexJustify::Center);
  return card;
}

void QuickTab::addSingleRow(Flex& card, std::unique_ptr<Button> row) {
  // The row is the whole card, so its press fill covers the card instead of an inset rectangle.
  const float s = contentScale();
  card.setPadding(0.0F);
  row->setPadding(Tokens::componentTilePadding * s);
  row->setRadius(Tokens::componentTileRadius * s);
  row->setFlexGrow(1.0F);
  card.addChild(std::move(row));
}

std::unique_ptr<Button> QuickTab::makeToggleRow(
    ToggleRow& row, std::string_view detailTab, float iconTextGap, bool chevron
) {
  const float s = contentScale();
  auto container = std::make_unique<Button>();
  container->setVariant(ButtonVariant::Ghost);
  container->setDirection(FlexDirection::Horizontal);
  container->setAlign(FlexAlign::Center);
  container->setPadding(0.0F);
  container->setMinHeight(kRoundIcon * s);
  container->setGap(iconTextGap * s);
  container->setContentAlign(ButtonContentAlign::Start);
  Shortcut* shortcut = row.shortcut.get();
  // An empty detail tab means the card itself is the toggle.
  container->setOnClick([this, shortcut, tab = std::string(detailTab), chevron] {
    if (!tab.empty() && !chevron) {
      m_openTab(tab);
    } else if (shortcut != nullptr) {
      shortcut->onClick();
    }
  });
  const std::string glyph = !row.glyph.empty() ? row.glyph : std::string(shortcut != nullptr ? shortcut->currentIcon() : "");
  auto icon = makeGlyphButton(row.icon, glyph, s, [shortcut] {
    if (shortcut != nullptr) {
      shortcut->onClick();
    }
  });
  icon->setZIndex(kAboveCardArea);
  icon->setStateFillless(true);
  attachCardMotion(*container, panelCardOpacity(), icon.get());
  container->addChild(std::move(icon));

  auto text = std::make_unique<Flex>();
  text->setDirection(FlexDirection::Vertical);
  text->setAlign(FlexAlign::Start);
  text->setFlexGrow(1.0F);
  text->addChild(makeLabel(row.title, Tokens::typeHeadline, s, ColorRole::Label1));
  text->addChild(makeLabel(row.subtitle, Tokens::typeFootnote, s, ColorRole::Label2));
  container->addChild(std::move(text));
  if (chevron) {
    Button* unused = nullptr;
    auto open = makeGlyphButton(unused, "chevron-right", s, [this, tab = std::string(detailTab)] { m_openTab(tab); },
                                kChevronHit, ButtonVariant::Ghost);
    open->setGlyphSize(kChevronGlyph * s);
    open->setZIndex(kAboveCardArea);
    if (open->inputArea() != nullptr) {
      constexpr float kOutset = 6.0F;
      open->inputArea()->setHitTestOutset({.left = kOutset * s, .top = kOutset * s, .right = kOutset * s, .bottom = kOutset * s});
    }
    if (open->glyph() != nullptr) {
      open->glyph()->setColor(colorSpecFromRole(ColorRole::Label3));
    }
    row.chevron = open.get();
    container->addChild(std::move(open));
    auto spinner = std::make_unique<Spinner>();
    spinner->setSpinnerSize(kChevronGlyph * 1.5F * s);
    spinner->setVisible(false);
    row.spinner = spinner.get();
    container->addChild(std::move(spinner));
  }
  row.body = container.get();
  if (!detailTab.empty()) {
    container->setOnRightClick([this, tab = std::string(detailTab)] { m_openTab(tab); });
  }
  return container;
}

std::unique_ptr<Flex> QuickTab::makeTileCard(std::string_view type, bool plain) {
  ToggleRow row;
  row.id = std::string(type);
  row.shortcut = ShortcutRegistry::create(type, m_services.shortcutServices());
  if (row.shortcut == nullptr) {
    return nullptr;
  }
  std::string_view detail;
  if (plain) {
  } else if (type == "wifi") {
    detail = "detail-wifi";
  } else if (type == "bluetooth") {
    detail = "detail-bluetooth";
  } else if (type == "notification") {
    detail = "detail-focus";
  } else if (type == "mic_mute") {
    detail = "detail-sound";
  }
  if (type == "notification") {
    row.glyph = "moon";
  }
  auto card = makeCard(plain);
  card->setFlexGrow(1.0F);
  addSingleRow(*card, makeToggleRow(row, detail, Tokens::componentTileGap, !detail.empty()));
  row.title->setText(type == "notification" ? i18n::tr("control-center.quick.focus")
                     : type == "dark_mode"  ? i18n::tr("control-center.quick.dark-mode")
                                            : row.shortcut->defaultLabel());
  row.card = card.get();
  row.detail = std::string(detail);
  m_extras.push_back(std::move(row));
  return card;
}

std::unique_ptr<Flex> QuickTab::makePill(std::string_view glyph, Slider*& slider, Label*& value) {
  const float s = contentScale();
  auto pill = std::make_unique<SliderPill>();
  pill->setDirection(FlexDirection::Horizontal);
  pill->setAlign(FlexAlign::Center);
  pill->setGap(Tokens::componentSliderInset * s);
  pill->setPadding(0.0F, Tokens::componentSliderInset * s, 0.0F, Tokens::componentSliderInset * s);
  pill->setMinHeight(kPillHeight * s);
  pill->setRadius(Tokens::componentSliderRadius * s);
  pill->setFill(colorSpecFromRole(ColorRole::SliderTrack));

  auto bar = std::make_unique<Slider>();
  bar->setRange(0.0, 1.0);
  bar->setStep(0.01);
  bar->setControlHeight(kPillHeight * s);
  bar->setFilled(true);
  bar->setTrackRadius(Tokens::componentSliderRadius * s);
  bar->setParticipatesInLayout(false);
  slider = bar.get();
  pill->slider = slider;
  pill->addChild(std::move(bar));

  auto icon = std::make_unique<Glyph>();
  icon->setGlyph(glyph);
  icon->setGlyphSize(Tokens::componentRoundIconGlyph * s);
  Glyph* iconPtr = icon.get();
  pill->addChild(std::move(icon));

  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  pill->addChild(std::move(spacer));

  auto label = makeLabel(value, Tokens::typeFootnote, s, ColorRole::Label2);
  label->setMaxWidth(120.0F * s);
  Label* labelPtr = label.get();
  pill->addChild(std::move(label));
  // The symbol and the value use on_slider_fill only where the level covers them; over the bare track they use label_2.
  const auto restyle = [pillPtr = pill.get(), iconPtr, labelPtr, s] {
    const float level = pillPtr->slider->normalizedValue() * pillPtr->width();
    const float inset = Tokens::componentSliderInset * s;
    const bool iconCovered = level >= inset + iconPtr->width() * 0.5F;
    const bool valueCovered = level >= pillPtr->width() - inset - labelPtr->width() * 0.5F;
    iconPtr->setColor(colorSpecFromRole(iconCovered ? ColorRole::OnSliderFill : ColorRole::Label2));
    labelPtr->setColor(colorSpecFromRole(valueCovered ? ColorRole::OnSliderFill : ColorRole::Label2));
  };
  slider->setOnLevelChanged(restyle);
  pill->restyle = restyle;
  return pill;
}

std::unique_ptr<Flex> QuickTab::create() {
  const float s = contentScale();
  m_extras.clear();
  m_editing = false;
  m_dragIndex = -1;
  m_pressedIndex = -1;
  m_pressedBadge = false;
  m_editReflowPending = false;
  m_editRebuildPending = false;
  m_editArea = nullptr;
  m_galleryArea = nullptr;
  m_editCards.clear();
  m_editRows.clear();
  m_editHalf.clear();
  m_galleryHalf.clear();
  m_galleryIds.clear();
  m_galleryCards.clear();
  m_editOrder.clear();
  m_page = 0;
  m_pageStep = 0.0F;
  m_halfWidth.clear();
  auto root = std::make_unique<Flex>();
  m_root = root.get();
  root->setDirection(FlexDirection::Vertical);
  root->setAlign(FlexAlign::Stretch);
  root->setGap(Tokens::componentSheetGap * s);

  m_pages.clear();
  m_dots.clear();
  m_pager = nullptr;
  auto pager = std::make_unique<ScrollView>();
  pager->setOrientation(ScrollOrientation::Horizontal);
  pager->setScrollbarVisible(false);
  pager->setViewportPaddingH(0.0F);
  pager->setViewportPaddingV(0.0F);
  pager->setContentScale(s);
  pager->setOnScrollChanged([this](float offset) { refreshDots(offset); });
  m_pager = pager.get();
  Flex* strip = pager->content();
  strip->setDirection(FlexDirection::Horizontal);
  strip->setAlign(FlexAlign::Stretch);
  strip->setGap(2.0F * Tokens::componentSheetPadding * s);
  root->addChild(std::move(pager));
  root->addChild(makeDots());
  buildPages(tileOrder());

  auto brightness = makePill("sun", m_brightnessSlider, m_brightnessValue);
  m_brightnessPill = brightness.get();
  m_brightnessSlider->setOnValueChanged([this](double value) {
    if (m_syncingSliders) {
      return;
    }
    if (m_services.brightness == nullptr) {
      return;
    }
    for (const BrightnessDisplay& display : m_services.brightness->displays()) {
      if (display.controllable) {
        m_services.brightness->setBrightness(display.id, static_cast<float>(value));
      }
    }
    m_brightnessValue->setText(percent(static_cast<float>(value)));
  });
  root->addChild(std::move(brightness));

  auto volume = makePill("volume", m_volumeSlider, m_volumeValue);
  m_volumeSlider->setOnValueChanged([this](double value) {
    if (m_syncingSliders) {
      return;
    }
    if (m_services.audio != nullptr) {
      if (const AudioNode* sink = m_services.audio->defaultSink(); sink != nullptr) {
        m_services.audio->setSinkVolume(sink->id, static_cast<float>(value));
      }
    }
  });
  auto volumeRow = std::make_unique<Flex>();
  volumeRow->setDirection(FlexDirection::Horizontal);
  volumeRow->setAlign(FlexAlign::Center);
  volumeRow->setGap(Tokens::componentSheetGap * s);
  volume->setFlexGrow(1.0F);
  volumeRow->addChild(std::move(volume));
  auto output = makeGlyphButton(m_outputButton, "device-speaker", s, [this] { m_openTab("detail-sound"); }, kPillHeight,
                                ButtonVariant::Tonal);
  m_outputButton->setRadius(Tokens::componentSliderRadius * s);
  volumeRow->addChild(std::move(output));
  root->addChild(std::move(volumeRow));

  auto media = makeCard();
  media->setDirection(FlexDirection::Horizontal);
  media->setAlign(FlexAlign::Center);
  m_mediaCard = media.get();
  media->setPadding(Tokens::componentMediaPadding * s);
  media->setMinHeight(Tokens::componentMediaHeight * s);
  // fill_3 shows while there is no artwork.
  const float art = Tokens::componentMediaArt * s;
  auto artBox = std::make_unique<Flex>();
  artBox->setFill(colorSpecFromRole(ColorRole::Fill3));
  artBox->setRadius(Tokens::componentMediaArtRadius * s);
  artBox->setMinWidth(art);
  artBox->setMinHeight(art);
  auto artImage = std::make_unique<Image>();
  artImage->setFit(ImageFit::Cover);
  artImage->setRadius(Tokens::componentMediaArtRadius * s);
  artImage->setSize(art, art);
  artImage->setParticipatesInLayout(false);
  m_mediaArt = artImage.get();
  artBox->addChild(std::move(artImage));
  media->addChild(std::move(artBox));
  auto mediaText = std::make_unique<Flex>();
  mediaText->setDirection(FlexDirection::Vertical);
  mediaText->setAlign(FlexAlign::Start);
  mediaText->setFlexGrow(1.0F);
  mediaText->addChild(makeLabel(m_mediaTitle, Tokens::typeHeadline, s, ColorRole::Label1));
  mediaText->addChild(makeLabel(m_mediaArtist, Tokens::typeFootnote, s, ColorRole::Label2));
  media->addChild(std::move(mediaText));
  // Bare glyphs; each button is glyph + gap wide, so glyph centers sit glyph + controls_gap apart as in the reference.
  TOKENS_LIVE(kMediaControl, Tokens::componentRoundIconGlyph + Tokens::componentMediaControlsGap);
  auto controls = std::make_unique<Flex>();
  controls->setDirection(FlexDirection::Horizontal);
  controls->setAlign(FlexAlign::Center);
  Button* unused = nullptr;
  controls->addChild(makeGlyphButton(unused, "player-skip-back", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->previousActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  controls->addChild(makeGlyphButton(m_playButton, "player-play", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->playPauseActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  controls->addChild(makeGlyphButton(unused, "player-skip-forward", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->nextActive();
    }
  }, kMediaControl, ButtonVariant::Ghost));
  media->addChild(std::move(controls));
  root->addChild(std::move(media));

  auto phone = makeCard();
  phone->setDirection(FlexDirection::Horizontal);
  phone->setAlign(FlexAlign::Center);
  m_phoneCard = phone.get();
  Button* phoneIcon = nullptr;
  phone->addChild(makeGlyphButton(phoneIcon, "device-mobile", s, {}, kRoundIcon, ButtonVariant::Primary));
  auto phoneText = std::make_unique<Flex>();
  phoneText->setDirection(FlexDirection::Vertical);
  phoneText->setAlign(FlexAlign::Start);
  phoneText->setFlexGrow(1.0F);
  phoneText->addChild(makeLabel(m_phoneName, Tokens::typeHeadline, s, ColorRole::Label1));
  phoneText->addChild(makeLabel(m_phoneStatus, Tokens::typeFootnote, s, ColorRole::Label2));
  phone->addChild(std::move(phoneText));
  auto actions = std::make_unique<Flex>();
  actions->setDirection(FlexDirection::Horizontal);
  actions->setAlign(FlexAlign::Center);
  actions->setGap(Tokens::componentRoundIconActionGap * s);
  actions->addChild(makeGlyphButton(unused, "bell-ringing", s, [this] {
    if (m_phoneRing != nullptr) {
      m_phoneRing->onClick();
    }
  }, Tokens::componentRoundIconAction));
  actions->addChild(makeGlyphButton(unused, "screen-share", s, [this] {
    if (m_services.link != nullptr && !m_phoneId.empty()) {
      m_services.link->mirror(m_phoneId);
    }
  }, Tokens::componentRoundIconAction));
  phone->addChild(std::move(actions));
  root->addChild(std::move(phone));

  root->addChild(makeFooter());
  m_normalParts.clear();
  for (const auto& child : root->children()) {
    m_normalParts.push_back(child.get());
  }
  buildEditParts(*root);

  return root;
}

std::unique_ptr<Flex> QuickTab::makeDots() {
  const float s = contentScale();
  auto row = std::make_unique<Flex>();
  row->setDirection(FlexDirection::Horizontal);
  row->setAlign(FlexAlign::Center);
  row->setJustify(FlexJustify::Center);
  row->setGap(Tokens::componentPageDotsGap * s);
  row->setMinHeight(Tokens::componentPageDotsRow * s);
  m_dotsRow = row.get();
  return row;
}

void QuickTab::fillDots(std::size_t pages) {
  const float s = contentScale();
  const float kDot = Tokens::componentPageDotsDot;
  const float kDotGap = Tokens::componentPageDotsGap;
  while (!m_dotsRow->children().empty()) {
    m_dotsRow->removeChild(m_dotsRow->children().back().get());
  }
  m_dots.clear();
  m_dotsRow->setVisible(pages > 1);
  for (std::size_t i = 0; i < pages; ++i) {
    auto dot = std::make_unique<Flex>();
    dot->setMinWidth(kDot * s);
    dot->setMinHeight(kDot * s);
    dot->setRadius(kDot * 0.5F * s);
    dot->setFill(colorSpecFromRole(ColorRole::Fill3));
    auto area = std::make_unique<InputArea>();
    area->setParticipatesInLayout(false);
    area->setFrameSize(kDot * s, kDot * s);
    area->setHitTestOutset({.left = kDotGap * 0.5F * s, .top = 6.0F * s, .right = kDotGap * 0.5F * s, .bottom = 6.0F * s});
    area->setOnClick([this, i](const InputArea::PointerData&) { goToPage(i); });
    dot->addChild(std::move(area));
    m_dots.push_back(dot.get());
    m_dotsRow->addChild(std::move(dot));
  }
}

void QuickTab::goToPage(std::size_t page) {
  if (m_pager != nullptr && m_pageStep > 0.0F && page < m_pages.size()) {
    m_pager->scrollToPage(page);
  }
}

const std::vector<std::string>& QuickTab::tileOrderDefault() {
  static const std::vector<std::string> kDefault = {"wifi",    "bluetooth",  "notification", "dark_mode",     "airplane",
                                                    "hotspot", "nightlight", "caffeine",     "screen-record", "mic_mute"};
  return kDefault;
}

std::vector<std::string> QuickTab::savedTileOrder() const {
  const auto* config = m_services.config;
  const auto& stored = config != nullptr ? config->config().controlCenter.quickTiles : std::vector<std::string>{};
  std::vector<std::string> order;
  for (const std::string& id : stored.empty() ? tileOrderDefault() : stored) {
    if (std::ranges::find(order, id) == order.end()) {
      order.push_back(id);
    }
  }
  return order;
}

std::vector<std::string> QuickTab::tileOrder() const {
  std::vector<std::string> order = savedTileOrder();
  if (std::ranges::none_of(order, [this](const std::string& id) { return tileAvailable(id); })) {
    return tileOrderDefault();
  }
  return order;
}

bool QuickTab::tileAvailable(std::string_view id) const {
  if (id == "airplane") {
    return hasRfkillRadios();
  }
  if (id == "hotspot") {
    return m_services.hotspot != nullptr;
  }
  if (id == "nightlight") {
    return m_services.nightLight != nullptr;
  }
  if (id == "caffeine") {
    return m_services.idleInhibitor != nullptr;
  }
  if (id == "screen-record") {
    return m_services.screenRecorder != nullptr;
  }
  if (id == "mic_mute") {
    return m_services.audio != nullptr && m_services.audio->defaultSource() != nullptr;
  }
  return id == "wifi" || id == "bluetooth" || id == "notification" || id == "dark_mode";
}

void QuickTab::buildPages(const std::vector<std::string>& order) {
  const float s = contentScale();
  Flex* strip = m_pager->content();
  while (!strip->children().empty()) {
    strip->removeChild(strip->children().back().get());
  }
  m_pages.clear();
  m_halfWidth.clear();
  m_extras.clear();
  std::vector<std::unique_ptr<Flex>> cards;
  for (const std::string& id : order) {
    if (!tileAvailable(id)) {
      continue;
    }
    if (auto card = makeTileCard(id); card != nullptr) {
      cards.push_back(std::move(card));
    }
  }
  constexpr std::size_t kTilesPerPage = 4;
  const std::size_t pages = std::max<std::size_t>(1, (cards.size() + kTilesPerPage - 1) / kTilesPerPage);
  for (std::size_t p = 0; p < pages; ++p) {
    auto page = std::make_unique<Flex>();
    page->setDirection(FlexDirection::Vertical);
    page->setAlign(FlexAlign::Stretch);
    page->setGap(Tokens::componentSheetGap * s);
    const std::size_t end = std::min(cards.size(), (p + 1) * kTilesPerPage);
    for (std::size_t i = p * kTilesPerPage; i < end; i += 2) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Stretch);
      row->setGap(Tokens::componentSheetGap * s);
      m_halfWidth.push_back(cards[i].get());
      row->addChild(std::move(cards[i]));
      if (i + 1 < end) {
        m_halfWidth.push_back(cards[i + 1].get());
        row->addChild(std::move(cards[i + 1]));
      } else {
        auto filler = std::make_unique<Flex>();
        m_halfWidth.push_back(filler.get());
        row->addChild(std::move(filler));
      }
      page->addChild(std::move(row));
    }
    m_pages.push_back(page.get());
    strip->addChild(std::move(page));
  }
  fillDots(m_pages.size());
}

std::unique_ptr<Flex> QuickTab::makeFooter() {
  const float s = contentScale();
  const float kFooterHeight = Tokens::componentButtonHeightM;
  auto footer = std::make_unique<Flex>();
  footer->setDirection(FlexDirection::Horizontal);
  footer->setAlign(FlexAlign::Center);
  footer->setMinHeight(kFooterHeight * s);
  footer->setJustify(FlexJustify::SpaceBetween);
  footer->setPadding(0.0F, Tokens::componentTilePadding * s, 0.0F, Tokens::componentTilePadding * s);
  m_footer = footer.get();

  const auto makeGlyph = [&](Glyph*& out) {
    auto glyph = std::make_unique<Glyph>();
    glyph->setGlyphSize(Tokens::componentRoundIconGlyph * s);
    glyph->setColor(colorSpecFromRole(ColorRole::Label2));
    out = glyph.get();
    return glyph;
  };

  auto battery = std::make_unique<Flex>();
  battery->setDirection(FlexDirection::Horizontal);
  battery->setAlign(FlexAlign::Center);
  battery->setGap(Tokens::spaceP8 * s);
  m_batteryBox = battery.get();
  battery->addChild(makeGlyph(m_batteryGlyph));
  battery->addChild(makeLabel(m_batteryText, Tokens::typeFootnote, s, ColorRole::Label2));
  footer->addChild(std::move(battery));

  m_profile = ShortcutRegistry::create("power_profile", m_services.shortcutServices());
  auto profile = std::make_unique<Button>();
  profile->setVariant(ButtonVariant::Secondary);
  profile->setDirection(FlexDirection::Horizontal);
  profile->setAlign(FlexAlign::Center);
  profile->setControlHeight(Tokens::componentButtonHeightS * s);
  profile->setRadius(Tokens::componentButtonRadius * s);
  profile->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
  profile->setGap(Tokens::componentButtonGap * s);
  profile->setOnClick([this] { m_openTab("detail-power"); });
  profile->setOnRightClick([this] {
    if (m_profile != nullptr) {
      m_profile->onClick();
    }
  });
  m_profileBox = profile.get();
  profile->addChild(makeGlyph(m_profileGlyph));
  profile->addChild(makeLabel(m_profileText, Tokens::typeFootnote, s, ColorRole::Label1));
  footer->addChild(std::move(profile));

  const auto iconButton = [&](Button*& out, std::string_view glyph, std::function<void()> click) {
    Button* unused = nullptr;
    auto b = makeGlyphButton(out, glyph, s, std::move(click), Tokens::componentButtonHeightM,
                             ButtonVariant::Ghost);
    b->setRadius(Tokens::componentButtonRadius * s);
    footer->addChild(std::move(b));
  };
  iconButton(m_editButton, "pencil", [this] { setEditing(true); });
  m_editButton->setTooltip(i18n::tr("control-center.quick.edit-tooltip"));
  Button* settings = nullptr;
  iconButton(settings, "settings", [] { PanelManager::instance().openSettingsWindow(); });
  settings->setTooltip(i18n::tr("control-center.quick.settings-tooltip"));
  return footer;
}

void QuickTab::sizePages(Renderer& renderer, float width) {
  if (m_pager == nullptr || m_pages.empty()) {
    return;
  }
  m_pageStep = width + 2.0F * Tokens::componentSheetPadding * contentScale();
  m_pager->setPaging(m_pageStep);
  for (Flex* page : m_pages) {
    page->setMinWidth(width);
    page->setMaxWidth(width);
    page->setMinHeight(0.0F);
  }
  const float half = std::floor((width - Tokens::componentSheetGap * contentScale()) * 0.5F);
  for (Flex* cell : m_halfWidth) {
    cell->setMinWidth(half);
    cell->setMaxWidth(half);
  }
  for (const auto* cells : {&m_editHalf, &m_galleryHalf}) {
    for (Flex* cell : *cells) {
      cell->setMinWidth(half);
      cell->setMaxWidth(half);
    }
  }
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const float height = m_pages.front()->measure(renderer, constraints).height;
  for (Flex* page : m_pages) {
    page->setMinHeight(height);
  }
  m_pager->setMinHeight(height);
  m_pager->setMaxHeight(height);
}

void QuickTab::refreshDots(float offset) {
  if (m_pageStep <= 0.0F || m_pages.empty()) {
    return;
  }
  m_page = std::min(static_cast<std::size_t>(std::lround(std::max(offset, 0.0F) / m_pageStep)), m_pages.size() - 1);
  for (std::size_t i = 0; i < m_dots.size(); ++i) {
    m_dots[i]->setFill(colorSpecFromRole(i == m_page ? ColorRole::Label1 : ColorRole::Fill3));
  }
}

void QuickTab::syncPager() {
  if (m_pager == nullptr || m_pageStep <= 0.0F) {
    return;
  }
  refreshDots(m_pager->scrollOffset());
}

float QuickTab::measureHeight(Renderer& renderer, float width) {
  if (m_root == nullptr) {
    return 0.0F;
  }
  sizePages(renderer, width);
  m_root->setSize(width, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  return m_root->measure(renderer, constraints).height;
}

void QuickTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  sizePages(renderer, contentWidth);
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  if (m_editArea != nullptr) {
    m_editArea->setFrameSize(m_editGrid->width(), m_editGrid->height());
  }
  if (m_galleryArea != nullptr) {
    m_galleryArea->setFrameSize(m_editGallery->width(), m_editGallery->height());
  }
  m_editReflowPending = false;
}

void QuickTab::syncToggleRow(ToggleRow& row, const std::string& subtitle) {
  if (row.shortcut == nullptr || row.icon == nullptr) {
    return;
  }
  const bool active = row.shortcut->active();
  row.icon->setGlyph(!row.glyph.empty() ? row.glyph : row.shortcut->currentIcon());
  row.icon->setVariant(active ? ButtonVariant::Primary : ButtonVariant::Tonal);
  row.icon->setEnabled(row.shortcut->enabled());
  row.subtitle->setText(subtitle);
  if (row.body != nullptr) {
    row.body->setTooltip(row.title->text() + "\n" + subtitle);
  }
}

void QuickTab::syncExtras() {
  for (ToggleRow& row : m_extras) {
    Shortcut* shortcut = row.shortcut.get();
    std::string subtitle;
    if (shortcut->id() == "mic_mute") {
      subtitle = i18n::tr(shortcut->active() ? "control-center.quick.muted" : "control-center.quick.on");
    } else {
      const std::string label = shortcut->displayLabel();
      const bool named = shortcut->id() != "notification" && shortcut->id() != "dark_mode";
      subtitle = shortcut->active() && named && label != shortcut->defaultLabel()
          ? label
          : i18n::tr(shortcut->active() ? "control-center.quick.on" : "control-center.quick.off");
    }
    const bool connecting = shortcut->id() == "wifi" && m_services.network != nullptr && m_services.network->state().resolving;
    if (connecting) {
      subtitle = i18n::tr("control-center.quick.connecting");
    }
    syncToggleRow(row, subtitle);
    if (row.spinner != nullptr) {
      row.chevron->setVisible(!connecting);
      row.spinner->setVisible(connecting);
      connecting ? row.spinner->start() : row.spinner->stop();
    }
  }
}

void QuickTab::syncFooter() {
  const UPowerState* state = m_services.upower != nullptr ? &m_services.upower->state() : nullptr;
  const bool battery = state != nullptr && state->isPresent;
  const bool profile = m_services.powerProfiles != nullptr && !m_services.powerProfiles->activeProfile().empty();
  m_batteryBox->setVisible(battery);
  m_profileBox->setVisible(profile);
  m_footer->setVisible(true);
  if (battery) {
    m_batteryGlyph->setGlyph(batteryGlyphName(state->percentage, state->state));
    const std::string stateLabel = batteryStateLabel(state->state);
    m_batteryText->setText(
        stateLabel.empty() ? std::format("{:.0F}%", state->percentage)
                           : std::format("{:.0F}% · {}", state->percentage, stateLabel)
    );
  }
  if (profile) {
    m_profileGlyph->setGlyph(m_profile->displayIcon());
    m_profileText->setText(m_profile->displayLabel());
  }
}

void QuickTab::syncSliders() {
  if (m_brightnessSlider->dragging() || m_volumeSlider->dragging()) {
    return;
  }
  m_syncingSliders = true;
  const BrightnessDisplay* display = nullptr;
  if (m_services.brightness != nullptr) {
    for (const BrightnessDisplay& candidate : m_services.brightness->displays()) {
      if (candidate.controllable) {
        display = &candidate;
        break;
      }
    }
  }
  m_brightnessPill->setVisible(display != nullptr);
  if (display != nullptr) {
    m_brightnessSlider->setValue(display->brightness);
    m_brightnessValue->setText(percent(display->brightness));
  }

  const AudioNode* sink = m_services.audio != nullptr ? m_services.audio->defaultSink() : nullptr;
  m_volumeSlider->setEnabled(sink != nullptr);
  m_volumeSlider->setValue(sink != nullptr ? sink->volume : 0.0);
  m_volumeValue->setText(sink == nullptr ? std::string{} : percent(sink->volume));

  m_syncingSliders = false;
}

void QuickTab::syncMedia(Renderer& renderer) {
  const auto player = m_services.mpris != nullptr ? m_services.mpris->activePlayer() : std::nullopt;
  m_mediaCard->setVisible(player.has_value());
  if (!player.has_value()) {
    return;
  }
  m_mediaTitle->setText(player->title);
  m_mediaArtist->setText(joinedArtists(player->artists));
  m_playButton->setGlyph(player->playbackStatus == "Playing" ? "player-pause" : "player-play");

  const std::string url = mpris::effectiveArtUrl(*player);
  if (url == m_mediaArtUrl && (url.empty() || m_mediaArt->hasImage())) {
    return;
  }
  const std::string path = mpris::resolveArtworkSource(
      m_services.httpClient, m_pendingArt, url, [this] {
        m_mediaArtUrl.clear();
        PanelManager::instance().refresh();
      }, m_aliveGuard
  );
  const auto decode = static_cast<int>(std::ceil(Tokens::componentMediaArt * contentScale() * 2.0F));
  if (path.empty() || !m_mediaArt->setSourceFile(renderer, path, decode, true, true)) {
    m_mediaArt->clear(renderer);
    m_mediaArtUrl = path.empty() && !url.empty() ? std::string{} : url;
    return;
  }
  m_mediaArtUrl = url;
}

void QuickTab::syncPhone() {
  const LinkDevice* phone = nullptr;
  if (m_services.link != nullptr) {
    for (const LinkDevice& device : m_services.link->devices()) {
      if (device.connected) {
        phone = &device;
        break;
      }
    }
  }
  m_phoneCard->setVisible(phone != nullptr);
  m_phoneId = phone != nullptr ? phone->id : std::string{};
  if (phone == nullptr) {
    return;
  }
  m_phoneName->setText(phone->name);
  std::string line;
  const auto append = [&line](const std::string& part) { line += line.empty() ? part : " · " + part; };
  if (const LinkStatus* status = m_services.link->status(phone->id); status != nullptr) {
    append(std::format("{}%", status->battery));
  }
  if (const std::uint32_t unread = m_services.link->notificationCount(phone->id); unread > 0) {
    append(i18n::tr("control-center.quick.notifications", "count", unread));
  }
  m_phoneStatus->setText(line);
}

float QuickTab::offsetIn(const Node* node, const Node* ancestor, bool y) {
  float total = 0.0F;
  for (; node != nullptr && node != ancestor; node = node->parent()) {
    total += y ? node->y() : node->x();
  }
  return total;
}

namespace {
  // Removes every child except `keep`: the persistent input area outlives the rebuilds its callbacks trigger.
  void clearChildren(Node& node, const Node* keep = nullptr) {
    std::vector<Node*> doomed;
    for (const auto& child : node.children()) {
      if (child.get() != keep) {
        doomed.push_back(child.get());
      }
    }
    for (Node* child : doomed) {
      node.removeChild(child);
    }
  }

  std::unique_ptr<Flex> makeBadge(bool add, float s) {
    const float size = (add ? Tokens::componentEditBadgeAdd : Tokens::componentEditBadgeRemove) * s;
    auto badge = std::make_unique<Flex>();
    badge->setDirection(FlexDirection::Horizontal);
    badge->setAlign(FlexAlign::Center);
    badge->setJustify(FlexJustify::Center);
    badge->setFill(colorSpecFromRole(add ? ColorRole::Accent : ColorRole::Label2));
    badge->setRadius(size * 0.5F);
    badge->setParticipatesInLayout(false);
    badge->setZIndex(3);
    auto glyph = std::make_unique<Glyph>();
    glyph->setGlyph(add ? "plus" : "minus");
    glyph->setGlyphSize((add ? 16.0F : 12.0F) * s);
    glyph->setColor(colorSpecFromRole(ColorRole::OnAccent));
    badge->addChild(std::move(glyph));
    return badge;
  }
} // namespace

void QuickTab::buildEditParts(Flex& root) {
  const float s = contentScale();
  auto header = std::make_unique<Flex>();
  header->setDirection(FlexDirection::Horizontal);
  header->setAlign(FlexAlign::Center);
  header->setMinHeight(Tokens::componentButtonHeightM * s);
  header->setPadding(0.0F, Tokens::componentTilePadding * s, 0.0F, Tokens::componentTilePadding * s);
  header->setVisible(false);
  Label* title = nullptr;
  auto titleLabel = makeLabel(title, Tokens::typeTitle3, s, ColorRole::Label1);
  titleLabel->setFlexGrow(1.0F);
  title->setText(i18n::tr("control-center.quick.edit-controls"));
  header->addChild(std::move(titleLabel));
  auto done = std::make_unique<Button>();
  done->setVariant(ButtonVariant::Primary);
  done->setText(i18n::tr("control-center.quick.done"));
  done->setControlHeight(Tokens::componentButtonHeightS * s);
  done->setRadius(Tokens::componentButtonHeightS * 0.5F * s);
  done->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
  done->setOnClick([this] { setEditing(false); });
  header->addChild(std::move(done));
  m_editHeader = header.get();
  root.addChild(std::move(header));

  auto grid = std::make_unique<Flex>();
  grid->setDirection(FlexDirection::Vertical);
  grid->setAlign(FlexAlign::Stretch);
  grid->setGap(Tokens::componentSheetGap * s);
  grid->setVisible(false);
  m_editGrid = grid.get();
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(10);
  m_editArea = area.get();
  area->setOnPress([this, s](const InputArea::PointerData& d) {
    if (d.button != BTN_LEFT) {
      return;
    }
    if (d.pressed) {
      m_pressedIndex = editSlotAt(d.localX, d.localY);
      m_pressedBadge = false;
      m_dragMoved = false;
      m_pressX = d.localX;
      m_pressY = d.localY;
      if (m_pressedIndex >= 0) {
        const Flex* card = m_editCards[static_cast<std::size_t>(m_pressedIndex)];
        const float reach = (Tokens::componentEditBadgeRemove - Tokens::componentEditBadgeInset) * s;
        const float cx = offsetIn(card, m_editGrid, false);
        const float cy = offsetIn(card, m_editGrid, true);
        m_pressedBadge = d.localX < cx + reach + 4.0F * s && d.localY < cy + reach + 4.0F * s;
      }
      return;
    }
    const int index = m_pressedIndex;
    const bool badge = m_pressedBadge;
    if (m_dragIndex >= 0 && static_cast<std::size_t>(m_dragIndex) < m_editCards.size()) {
      m_editCards[static_cast<std::size_t>(m_dragIndex)]->setScale(1.0F);
    }
    m_dragIndex = -1;
    m_pressedIndex = -1;
    if (index >= 0 && badge && !m_dragMoved && m_editOrder.size() > 1) {
      m_editOrder.erase(m_editOrder.begin() + index);
      m_editRebuildPending = true;
      PanelManager::instance().refresh();
    }
  });
  area->setOnMotion([this, s](const InputArea::PointerData& d) {
    if (m_pressedIndex < 0 || m_pressedBadge || m_editArea == nullptr || !m_editArea->pressed() || m_editReflowPending) {
      return;
    }
    if (m_dragIndex < 0) {
      if (std::hypot(d.localX - m_pressX, d.localY - m_pressY) < 4.0F * s) {
        return;
      }
      m_dragIndex = m_pressedIndex;
      m_dragMoved = true;
      Flex* card = m_editCards[static_cast<std::size_t>(m_dragIndex)];
      card->setTransformOrigin(card->width() * 0.5F, card->height() * 0.5F);
      card->setScale(Tokens::componentEditDragScale);
      card->setZIndex(5);
    }
    const int slot = editSlotAt(d.localX, d.localY);
    if (slot < 0 || slot == m_dragIndex) {
      return;
    }
    const auto from = static_cast<std::ptrdiff_t>(m_dragIndex);
    const auto to = static_cast<std::ptrdiff_t>(slot);
    if (from < to) {
      std::rotate(m_editOrder.begin() + from, m_editOrder.begin() + from + 1, m_editOrder.begin() + to + 1);
      std::rotate(m_editCards.begin() + from, m_editCards.begin() + from + 1, m_editCards.begin() + to + 1);
    } else {
      std::rotate(m_editOrder.begin() + to, m_editOrder.begin() + from, m_editOrder.begin() + from + 1);
      std::rotate(m_editCards.begin() + to, m_editCards.begin() + from, m_editCards.begin() + from + 1);
    }
    m_dragIndex = slot;
    m_editReflowPending = true;
    reflowEditGrid();
    m_editCards[static_cast<std::size_t>(slot)]->setScale(Tokens::componentEditDragScale);
    PanelManager::instance().requestLayout();
  });
  grid->addChild(std::move(area));
  root.addChild(std::move(grid));

  auto gallery = std::make_unique<Flex>();
  gallery->setDirection(FlexDirection::Vertical);
  gallery->setAlign(FlexAlign::Stretch);
  gallery->setGap(Tokens::componentSheetGap * s);
  gallery->setVisible(false);
  m_editGallery = gallery.get();
  auto galleryArea = std::make_unique<InputArea>();
  galleryArea->setParticipatesInLayout(false);
  galleryArea->setZIndex(10);
  m_galleryArea = galleryArea.get();
  galleryArea->setOnClick([this](const InputArea::PointerData& d) {
    for (std::size_t i = 0; i < m_galleryCards.size(); ++i) {
      const Flex* card = m_galleryCards[i];
      const float cx = offsetIn(card, m_editGallery, false);
      const float cy = offsetIn(card, m_editGallery, true);
      if (d.localX >= cx && d.localX < cx + card->width() && d.localY >= cy && d.localY < cy + card->height()) {
        m_editOrder.push_back(m_galleryIds[i]);
        m_editRebuildPending = true;
        PanelManager::instance().refresh();
        return;
      }
    }
  });
  gallery->addChild(std::move(galleryArea));
  root.addChild(std::move(gallery));
}

void QuickTab::reflowEditGrid() {
  const float s = contentScale();
  std::vector<std::unique_ptr<Flex>> held;
  for (Flex* card : m_editCards) {
    auto node = card->parent()->removeChild(card);
    held.emplace_back(static_cast<Flex*>(node.release()));
  }
  clearChildren(*m_editGrid, m_editArea);
  m_editRows.clear();
  m_editCards.clear();
  m_editHalf.clear();
  for (std::size_t i = 0; i < held.size(); i += 2) {
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Stretch);
    row->setGap(Tokens::componentSheetGap * s);
    m_editCards.push_back(held[i].get());
    m_editHalf.push_back(held[i].get());
    row->addChild(std::move(held[i]));
    if (i + 1 < held.size()) {
      m_editCards.push_back(held[i + 1].get());
      m_editHalf.push_back(held[i + 1].get());
      row->addChild(std::move(held[i + 1]));
    } else {
      auto filler = std::make_unique<Flex>();
      m_editHalf.push_back(filler.get());
      row->addChild(std::move(filler));
    }
    m_editRows.push_back(row.get());
    m_editGrid->addChild(std::move(row));
  }
}

int QuickTab::editSlotAt(float x, float y) const {
  const float pad = Tokens::componentSheetGap * contentScale() * 0.5F;
  for (std::size_t i = 0; i < m_editCards.size(); ++i) {
    const Flex* card = m_editCards[i];
    const float cx = offsetIn(card, m_editGrid, false);
    const float cy = offsetIn(card, m_editGrid, true);
    if (x >= cx - pad && x < cx + card->width() + pad && y >= cy - pad && y < cy + card->height() + pad) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void QuickTab::rebuildEdit() {
  const float s = contentScale();
  m_extras.clear();
  m_galleryCards.clear();
  m_galleryIds.clear();
  m_editCards.clear();
  clearChildren(*m_editGrid, m_editArea);
  clearChildren(*m_editGallery, m_galleryArea);
  m_editRows.clear();
  m_editHalf.clear();
  m_galleryHalf.clear();

  std::vector<std::unique_ptr<Flex>> cards;
  std::vector<std::string> built; // ids that have a card: card index == order index
  for (const std::string& id : m_editOrder) {
    auto card = makeTileCard(id, true);
    if (card == nullptr) {
      continue;
    }
    built.push_back(id);
    auto* badged = static_cast<BadgedCard*>(card.get());
    badged->scale = s;
    auto badge = makeBadge(false, s);
    badged->badge = badge.get();
    card->addChild(std::move(badge));
    m_editCards.push_back(card.get());
    cards.push_back(std::move(card));
  }
  m_editOrder = std::move(built);
  m_editCards.clear();
  // Park the cards in a temporary row so reflowEditGrid() can take them over.
  auto parking = std::make_unique<Flex>();
  for (auto& card : cards) {
    m_editCards.push_back(card.get());
    parking->addChild(std::move(card));
  }
  m_editGrid->addChild(std::move(parking));
  reflowEditGrid();

  std::vector<std::unique_ptr<Flex>> gallery;
  for (const std::string& id : tileOrderDefault()) {
    if (!tileAvailable(id) || std::ranges::find(m_editOrder, id) != m_editOrder.end()) {
      continue;
    }
    auto card = makeTileCard(id, true);
    if (card == nullptr) {
      continue;
    }
    auto* badged = static_cast<BadgedCard*>(card.get());
    badged->scale = s;
    badged->addMode = true;
    card->setFill(colorSpecFromRole(ColorRole::Fill1, 0.0F));
    card->setBorder(colorSpecFromRole(ColorRole::Separator), 1.0F);
    card->setOpacity(0.72F);
    auto badge = makeBadge(true, s);
    badged->badge = badge.get();
    card->addChild(std::move(badge));
    m_galleryIds.push_back(id);
    m_galleryCards.push_back(card.get());
    gallery.push_back(std::move(card));
  }
  if (!gallery.empty()) {
    Label* title = nullptr;
    auto titleLabel = makeLabel(title, Tokens::typeFootnote, s, ColorRole::Label1);
    title->setText(i18n::tr("control-center.quick.add-controls"));
    m_editGallery->addChild(std::move(titleLabel));
    for (std::size_t i = 0; i < gallery.size(); i += 2) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Stretch);
      row->setGap(Tokens::componentSheetGap * s);
      m_galleryHalf.push_back(gallery[i].get());
      row->addChild(std::move(gallery[i]));
      if (i + 1 < gallery.size()) {
        m_galleryHalf.push_back(gallery[i + 1].get());
        row->addChild(std::move(gallery[i + 1]));
      } else {
        auto filler = std::make_unique<Flex>();
        m_galleryHalf.push_back(filler.get());
        row->addChild(std::move(filler));
      }
      m_editGallery->addChild(std::move(row));
    }
  }
  Label* hint = nullptr;
  auto hintLabel = makeLabel(hint, Tokens::typeFootnote, s, ColorRole::Label3);
  hint->setText(i18n::tr("control-center.quick.edit-hint"));
  m_editGallery->addChild(std::move(hintLabel));
  syncExtras();
  m_editReflowPending = true;
  PanelManager::instance().requestLayout();
}

void QuickTab::saveEdit() {
  if (m_services.config == nullptr) {
    return;
  }
  const std::vector<std::string> saved = savedTileOrder();
  std::vector<std::string> next = m_editOrder;
  for (std::size_t i = 0; i < saved.size(); ++i) {
    const std::string& id = saved[i];
    if (!tileAvailable(id) && std::ranges::find(tileOrderDefault(), id) != tileOrderDefault().end() &&
        std::ranges::find(next, id) == next.end()) {
      next.insert(next.begin() + static_cast<std::ptrdiff_t>(std::min(i, next.size())), id);
    }
  }
  if (next != saved) {
    m_services.config->setOverride({"control_center", "quick_tiles"}, next);
  }
}

void QuickTab::onClose() {
  if (m_editing) {
    saveEdit();
    m_editing = false;
  }
}

void QuickTab::setEditing(bool editing) {
  if (m_editing == editing || m_root == nullptr || m_editGrid == nullptr) {
    return;
  }
  m_editing = editing;
  m_editRebuildPending = false;
  if (editing) {
    m_editOrder.clear();
    for (const std::string& id : tileOrder()) {
      if (tileAvailable(id)) {
        m_editOrder.push_back(id);
      }
    }
    rebuildEdit();
  } else {
    saveEdit();
    m_extras.clear();
    clearChildren(*m_editGrid, m_editArea);
    clearChildren(*m_editGallery, m_galleryArea);
    m_editCards.clear();
    m_editRows.clear();
    m_editHalf.clear();
    m_galleryHalf.clear();
    m_galleryCards.clear();
    m_galleryIds.clear();
    buildPages(m_editOrder);
    m_pager->setScrollOffset(0.0F);
    m_page = 0;
    refreshDots(0.0F);
    syncSliders();
    syncPhone();
    if (m_mediaCard != nullptr) {
      m_mediaCard->setVisible(m_services.mpris != nullptr && m_services.mpris->activePlayer().has_value());
    }
  }
  for (Node* part : m_normalParts) {
    // The dots row, the sliders, media and phone cards own their visibility (fillDots and the sync functions).
    if (!editing && (part == m_dotsRow || part == m_brightnessPill || part == m_mediaCard || part == m_phoneCard)) {
      continue;
    }
    part->setVisible(!editing);
  }
  m_editHeader->setVisible(editing);
  m_editGrid->setVisible(editing);
  m_editGallery->setVisible(editing);
  PanelManager::instance().requestLayout();
}

bool QuickTab::moveFocus(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= m_extras.size() || m_extras[static_cast<std::size_t>(index)].body == nullptr) {
    return true;
  }
  goToPage(static_cast<std::size_t>(index) / 4);
  PanelManager::instance().inputDispatcher().setFocus(m_extras[static_cast<std::size_t>(index)].body->inputArea());
  return true;
}

bool QuickTab::handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  (void)modifiers;
  if (!pressed || preedit) {
    return false;
  }
  if (m_editing) {
    if (sym == XKB_KEY_Escape) {
      setEditing(false);
      return true;
    }
    return false;
  }
  const InputArea* focused = PanelManager::instance().inputDispatcher().focusedArea();
  int at = -1;
  for (std::size_t i = 0; i < m_extras.size(); ++i) {
    if (m_extras[i].body != nullptr && focused != nullptr && m_extras[i].body->inputArea() == focused) {
      at = static_cast<int>(i);
    }
  }
  if (at >= 0) {
    const auto index = static_cast<std::size_t>(at);
    const bool leftColumn = at % 2 == 0;
    const int rowInPage = (at % 4) / 2;
    switch (sym) {
    case XKB_KEY_Left:
      return leftColumn ? (at >= 4 ? moveFocus(at - 3) : true) : moveFocus(at - 1);
    case XKB_KEY_Right:
      return !leftColumn ? (static_cast<std::size_t>(at + 3) < m_extras.size() ? moveFocus(at + 3) : true)
                         : (index + 1 < m_extras.size() ? moveFocus(at + 1) : true);
    case XKB_KEY_Up:
      return rowInPage == 1 ? moveFocus(at - 2) : true;
    case XKB_KEY_Down:
      return rowInPage == 0 && index + 2 < m_extras.size() && (at + 2) / 4 == at / 4 ? moveFocus(at + 2) : false;
    case XKB_KEY_Menu:
      if (!m_extras[index].detail.empty()) {
        m_openTab(m_extras[index].detail);
      }
      return true;
    default:
      break;
    }
  }
  if (sym == XKB_KEY_Page_Down) {
    goToPage(m_page + 1);
    return true;
  }
  if (sym == XKB_KEY_Page_Up && m_page > 0) {
    goToPage(m_page - 1);
    return true;
  }
  return false;
}


void QuickTab::doUpdate(Renderer& renderer) {
  if (m_editRebuildPending) {
    m_editRebuildPending = false;
    rebuildEdit();
  }
  if (m_editing) {
    syncExtras();
    return;
  }
  syncExtras();
  syncSliders();
  syncFooter();
  syncPager();
  syncMedia(renderer);
  syncPhone();
}
