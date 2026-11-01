#include "shell/control_center/tabs/quick_tab.h"

#include "dbus/link/link_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/panel/panel_manager.h"
#include "system/brightness_service.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <format>
#include <utility>

namespace {

  // Anatomy: docs/design-system.md "Control center"; values: protocol/design-tokens.toml [component.*].
  constexpr float kRoundIcon = Tokens::componentRoundIconSize;
  constexpr float kPillHeight = Tokens::componentSliderHeight;

  // The slider fills the whole pill; the icon and the value label draw over it.
  class SliderPill : public Flex {
  public:
    Slider* slider = nullptr;

  protected:
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      if (slider != nullptr) {
        slider->arrange(renderer, LayoutRect{.width = width(), .height = height()});
      }
    }
  };

  std::string percent(float value) { return std::format("{}%", static_cast<int>(std::lround(value * 100.0F))); }

  std::unique_ptr<Label> makeLabel(Label*& out, const Tokens::Type& type, float scale, ColorRole role) {
    auto label = std::make_unique<Label>();
    label->setFontSize(type.size * scale);
    label->setLineHeight(type.size * type.lineHeight * scale);
    label->setFontWeight(static_cast<FontWeight>(type.weight));
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

} // namespace

QuickTab::QuickTab(const ControlCenterServices& services, OpenTabCallback openTab)
    : m_services(services), m_openTab(std::move(openTab)) {
  const ShortcutServices shortcuts = services.shortcutServices();
  m_wifi.shortcut = ShortcutRegistry::create("wifi", shortcuts);
  m_bluetooth.shortcut = ShortcutRegistry::create("bluetooth", shortcuts);
  m_focus.shortcut = ShortcutRegistry::create("notification", shortcuts);
  m_focus.glyph = "moon";
  m_darkMode.shortcut = ShortcutRegistry::create("dark_mode", shortcuts);
  m_phoneRing = ShortcutRegistry::create("phone", shortcuts);
}

std::unique_ptr<Flex> QuickTab::makeCard() {
  const float s = contentScale();
  auto card = std::make_unique<Flex>();
  control_center::applySectionCardStyle(*card, s, panelCardOpacity());
  // Cards on a material are fill-1 (docs/design-system.md), lighter than the sheet; the shared card style is fill-2.
  card->setFill(colorSpecFromRole(ColorRole::Fill1, panelCardOpacity()));
  card->clearBorder();
  card->setRadius(Tokens::componentCardRadius * s);
  card->setPadding(Tokens::componentCardPadding * s);
  card->setGap(Tokens::componentCardRowGap * s);
  card->setJustify(FlexJustify::Center);
  return card;
}

std::unique_ptr<Button> QuickTab::makeToggleRow(ToggleRow& row, std::string_view detailTab, float iconTextGap) {
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
  container->setOnClick([this, shortcut, tab = std::string(detailTab)] {
    if (!tab.empty()) {
      m_openTab(tab);
    } else if (shortcut != nullptr) {
      shortcut->onClick();
    }
  });
  const std::string glyph = !row.glyph.empty() ? row.glyph : std::string(shortcut != nullptr ? shortcut->currentIcon() : "");
  container->addChild(makeGlyphButton(row.icon, glyph, s, [shortcut] {
    if (shortcut != nullptr) {
      shortcut->onClick();
    }
  }));

  auto text = std::make_unique<Flex>();
  text->setDirection(FlexDirection::Vertical);
  text->setAlign(FlexAlign::Start);
  text->setFlexGrow(1.0F);
  text->addChild(makeLabel(row.title, Tokens::typeTitle, s, ColorRole::Label1));
  text->addChild(makeLabel(row.subtitle, Tokens::typeCaption, s, ColorRole::Label2));
  container->addChild(std::move(text));
  return container;
}

std::unique_ptr<Flex> QuickTab::makePill(std::string_view glyph, Slider*& slider, Label*& value) {
  const float s = contentScale();
  auto pill = std::make_unique<SliderPill>();
  pill->setDirection(FlexDirection::Horizontal);
  pill->setAlign(FlexAlign::Center);
  pill->setGap(Tokens::componentSliderIconInset * s);
  pill->setPadding(0.0F, Tokens::componentSliderValueInset * s, 0.0F, Tokens::componentSliderIconInset * s);
  pill->setMinHeight(kPillHeight * s);
  pill->setRadius(Tokens::componentSliderRadius * s);
  pill->setFill(colorSpecFromRole(ColorRole::Fill3, panelCardOpacity()));

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
  // The icon always sits on the white slider fill.
  icon->setColor(fixedColorSpec(Color{.a = 0.6F}));
  pill->addChild(std::move(icon));

  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  pill->addChild(std::move(spacer));

  auto label = makeLabel(value, Tokens::typeCaption, s, ColorRole::Label2);
  label->setMaxWidth(120.0F * s);
  pill->addChild(std::move(label));
  return pill;
}

std::unique_ptr<Flex> QuickTab::create() {
  const float s = contentScale();
  auto root = std::make_unique<Flex>();
  m_root = root.get();
  root->setDirection(FlexDirection::Vertical);
  root->setAlign(FlexAlign::Stretch);
  root->setGap(Tokens::componentSheetGap * s);

  auto top = std::make_unique<Flex>();
  top->setDirection(FlexDirection::Horizontal);
  top->setAlign(FlexAlign::Stretch);
  top->setGap(Tokens::componentSheetGap * s);

  auto connectivity = makeCard();
  connectivity->setFlexGrow(1.0F);
  connectivity->addChild(makeToggleRow(m_wifi, "network", Tokens::componentCardRowGap));
  connectivity->addChild(makeToggleRow(m_bluetooth, "bluetooth", Tokens::componentCardRowGap));
  top->addChild(std::move(connectivity));

  auto smallCards = std::make_unique<Flex>();
  smallCards->setDirection(FlexDirection::Vertical);
  smallCards->setAlign(FlexAlign::Stretch);
  smallCards->setGap(Tokens::componentSheetGap * s);
  smallCards->setFlexGrow(1.0F);
  auto focus = makeCard();
  focus->addChild(makeToggleRow(m_focus, "notifications", Tokens::componentCardSmallGap));
  smallCards->addChild(std::move(focus));
  auto dark = makeCard();
  dark->addChild(makeToggleRow(m_darkMode, "", Tokens::componentCardSmallGap));
  smallCards->addChild(std::move(dark));
  top->addChild(std::move(smallCards));
  root->addChild(std::move(top));

  auto brightness = makePill("sun", m_brightnessSlider, m_brightnessValue);
  m_brightnessPill = brightness.get();
  m_brightnessSlider->setOnValueChanged([this](double value) {
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
    if (m_services.audio != nullptr) {
      if (const AudioNode* sink = m_services.audio->defaultSink(); sink != nullptr) {
        m_services.audio->setSinkVolume(sink->id, static_cast<float>(value));
      }
    }
  });
  root->addChild(std::move(volume));

  auto media = makeCard();
  media->setDirection(FlexDirection::Horizontal);
  media->setAlign(FlexAlign::Center);
  m_mediaCard = media.get();
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
  mediaText->addChild(makeLabel(m_mediaTitle, Tokens::typeTitle, s, ColorRole::Label1));
  mediaText->addChild(makeLabel(m_mediaArtist, Tokens::typeCaption, s, ColorRole::Label2));
  media->addChild(std::move(mediaText));
  // Bare glyphs; each button is glyph + gap wide, so glyph centers sit glyph + controls_gap apart as in the reference.
  constexpr float kMediaControl = Tokens::componentRoundIconGlyph + Tokens::componentMediaControlsGap;
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
  phoneText->addChild(makeLabel(m_phoneName, Tokens::typeTitle, s, ColorRole::Label1));
  phoneText->addChild(makeLabel(m_phoneStatus, Tokens::typeCaption, s, ColorRole::Label2));
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

  m_wifi.title->setText(i18n::tr("control-center.quick.wifi"));
  m_bluetooth.title->setText(i18n::tr("control-center.quick.bluetooth"));
  m_focus.title->setText(i18n::tr("control-center.quick.focus"));
  m_darkMode.title->setText(i18n::tr("control-center.quick.dark-mode"));
  return root;
}

float QuickTab::measureHeight(Renderer& renderer, float width) {
  if (m_root == nullptr) {
    return 0.0F;
  }
  m_root->setSize(width, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  return m_root->measure(renderer, constraints).height;
}

void QuickTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
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
}

void QuickTab::syncSliders() {
  if (m_brightnessSlider->dragging() || m_volumeSlider->dragging()) {
    return;
  }
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
  m_volumeValue->setText(
      sink == nullptr ? std::string{} : (sink->description.empty() ? sink->name : sink->description)
  );
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

void QuickTab::doUpdate(Renderer& renderer) {
  const auto onOff = [](const Shortcut* shortcut) {
    return i18n::tr(shortcut != nullptr && shortcut->active() ? "control-center.quick.on" : "control-center.quick.off");
  };
  const auto labelOr = [&](const ToggleRow& row) {
    if (row.shortcut == nullptr) {
      return std::string();
    }
    const std::string label = row.shortcut->displayLabel();
    return row.shortcut->active() && label != row.shortcut->defaultLabel() ? label : onOff(row.shortcut.get());
  };
  syncToggleRow(m_wifi, labelOr(m_wifi));
  syncToggleRow(m_bluetooth, labelOr(m_bluetooth));
  syncToggleRow(m_focus, onOff(m_focus.shortcut.get()));
  syncToggleRow(m_darkMode, onOff(m_darkMode.shortcut.get()));
  syncSliders();
  syncMedia(renderer);
  syncPhone();
}
