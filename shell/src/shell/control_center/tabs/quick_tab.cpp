#include "shell/control_center/tabs/quick_tab.h"

#include "dbus/link/link_service.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "system/brightness_service.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <format>
#include <utility>

namespace {

  constexpr float kRoundIcon = 32.0F;
  constexpr float kPillHeight = 40.0F;

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

  std::unique_ptr<Label> makeLabel(Label*& out, float size, FontWeight weight, ColorRole role) {
    auto label = std::make_unique<Label>();
    label->setFontSize(size);
    label->setFontWeight(weight);
    label->setColor(colorSpecFromRole(role));
    label->setMaxLines(1);
    out = label.get();
    return label;
  }

  std::unique_ptr<Button> makeGlyphButton(Button*& out, std::string_view glyph, float scale, std::function<void()> click) {
    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Ghost);
    button->setGlyph(glyph);
    button->setGlyphSize(Style::fontSizeBody * scale);
    button->setControlHeight(kRoundIcon * scale);
    button->setRadius(kRoundIcon * 0.5F * scale);
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
  m_darkMode.shortcut = ShortcutRegistry::create("dark_mode", shortcuts);
  m_phoneRing = ShortcutRegistry::create("phone", shortcuts);
}

std::unique_ptr<Flex> QuickTab::makeCard() {
  auto card = std::make_unique<Flex>();
  control_center::applySectionCardStyle(*card, contentScale(), panelCardOpacity());
  return card;
}

std::unique_ptr<Button> QuickTab::makeToggleRow(ToggleRow& row, std::string_view detailTab) {
  const float s = contentScale();
  auto container = std::make_unique<Button>();
  container->setVariant(ButtonVariant::Ghost);
  container->setDirection(FlexDirection::Horizontal);
  container->setAlign(FlexAlign::Center);
  container->setGap(Style::spaceSm * s);
  container->setContentAlign(ButtonContentAlign::Start);
  container->setOnClick([this, tab = std::string(detailTab)] { m_openTab(tab); });

  Shortcut* shortcut = row.shortcut.get();
  container->addChild(makeGlyphButton(row.icon, shortcut != nullptr ? shortcut->currentIcon() : "", s, [shortcut] {
    if (shortcut != nullptr) {
      shortcut->onClick();
    }
  }));

  auto text = std::make_unique<Flex>();
  text->setDirection(FlexDirection::Vertical);
  text->setAlign(FlexAlign::Start);
  text->setFlexGrow(1.0F);
  text->addChild(makeLabel(row.title, Style::fontSizeBody * s, FontWeight::SemiBold, ColorRole::Label1));
  text->addChild(makeLabel(row.subtitle, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2));
  container->addChild(std::move(text));
  return container;
}

std::unique_ptr<Flex> QuickTab::makePill(std::string_view glyph, Slider*& slider, Label*& value) {
  const float s = contentScale();
  auto pill = std::make_unique<SliderPill>();
  pill->setDirection(FlexDirection::Horizontal);
  pill->setAlign(FlexAlign::Center);
  pill->setGap(Style::spaceSm * s);
  pill->setPadding(0.0F, Style::spaceMd * s);
  pill->setMinHeight(kPillHeight * s);
  pill->setRadius(kPillHeight * 0.5F * s);
  pill->setFill(colorSpecFromRole(ColorRole::Fill1, panelCardOpacity()));

  auto bar = std::make_unique<Slider>();
  bar->setRange(0.0, 1.0);
  bar->setStep(0.01);
  bar->setControlHeight(kPillHeight * s);
  bar->setFilled(true);
  bar->setParticipatesInLayout(false);
  bar->setOnDragEnd([this] { m_draggingSlider = false; });
  slider = bar.get();
  pill->slider = slider;
  pill->addChild(std::move(bar));

  auto icon = std::make_unique<Glyph>();
  icon->setGlyph(glyph);
  icon->setGlyphSize(Style::fontSizeBody * s);
  // The icon always sits on the white slider fill.
  icon->setColor(fixedColorSpec(Color{.a = 0.6F}));
  pill->addChild(std::move(icon));

  auto spacer = std::make_unique<Flex>();
  spacer->setFlexGrow(1.0F);
  pill->addChild(std::move(spacer));

  auto label = makeLabel(value, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2);
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
  root->setGap(Style::spaceSm * s);

  auto top = std::make_unique<Flex>();
  top->setDirection(FlexDirection::Horizontal);
  top->setAlign(FlexAlign::Stretch);
  top->setGap(Style::spaceSm * s);

  auto connectivity = makeCard();
  connectivity->setFlexGrow(1.0F);
  connectivity->addChild(makeToggleRow(m_wifi, "network"));
  connectivity->addChild(makeToggleRow(m_bluetooth, "bluetooth"));
  top->addChild(std::move(connectivity));

  auto smallCards = std::make_unique<Flex>();
  smallCards->setDirection(FlexDirection::Vertical);
  smallCards->setAlign(FlexAlign::Stretch);
  smallCards->setGap(Style::spaceSm * s);
  smallCards->setFlexGrow(1.0F);
  auto focus = makeCard();
  focus->addChild(makeToggleRow(m_focus, "notifications"));
  smallCards->addChild(std::move(focus));
  auto dark = makeCard();
  dark->addChild(makeToggleRow(m_darkMode, "home"));
  smallCards->addChild(std::move(dark));
  top->addChild(std::move(smallCards));
  root->addChild(std::move(top));

  auto brightness = makePill("sun", m_brightnessSlider, m_brightnessValue);
  m_brightnessPill = brightness.get();
  m_brightnessSlider->setOnValueChanged([this](double value) {
    m_draggingSlider = true;
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
    m_draggingSlider = true;
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
  auto mediaText = std::make_unique<Flex>();
  mediaText->setDirection(FlexDirection::Vertical);
  mediaText->setAlign(FlexAlign::Start);
  mediaText->setFlexGrow(1.0F);
  mediaText->addChild(makeLabel(m_mediaTitle, Style::fontSizeBody * s, FontWeight::SemiBold, ColorRole::Label1));
  mediaText->addChild(makeLabel(m_mediaArtist, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2));
  media->addChild(std::move(mediaText));
  Button* unused = nullptr;
  media->addChild(makeGlyphButton(unused, "player-skip-back", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->previousActive();
    }
  }));
  media->addChild(makeGlyphButton(m_playButton, "player-play", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->playPauseActive();
    }
  }));
  media->addChild(makeGlyphButton(unused, "player-skip-forward", s, [this] {
    if (m_services.mpris != nullptr) {
      m_services.mpris->nextActive();
    }
  }));
  root->addChild(std::move(media));

  auto phone = makeCard();
  phone->setDirection(FlexDirection::Horizontal);
  phone->setAlign(FlexAlign::Center);
  m_phoneCard = phone.get();
  Button* phoneIcon = nullptr;
  phone->addChild(makeGlyphButton(phoneIcon, "device-mobile", s, {}));
  phoneIcon->setVariant(ButtonVariant::Primary);
  auto phoneText = std::make_unique<Flex>();
  phoneText->setDirection(FlexDirection::Vertical);
  phoneText->setAlign(FlexAlign::Start);
  phoneText->setFlexGrow(1.0F);
  phoneText->addChild(makeLabel(m_phoneName, Style::fontSizeBody * s, FontWeight::SemiBold, ColorRole::Label1));
  phoneText->addChild(makeLabel(m_phoneStatus, Style::fontSizeCaption * s, FontWeight::Medium, ColorRole::Label2));
  phone->addChild(std::move(phoneText));
  phone->addChild(makeGlyphButton(unused, "bell-ringing", s, [this] {
    if (m_phoneRing != nullptr) {
      m_phoneRing->onClick();
    }
  }));
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
  row.icon->setGlyph(row.shortcut->currentIcon());
  row.icon->setVariant(active ? ButtonVariant::Primary : ButtonVariant::Default);
  row.icon->setEnabled(row.shortcut->enabled());
  row.subtitle->setText(subtitle);
}

void QuickTab::syncSliders() {
  if (m_draggingSlider) {
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

void QuickTab::syncMedia() {
  const auto player = m_services.mpris != nullptr ? m_services.mpris->activePlayer() : std::nullopt;
  m_mediaCard->setVisible(player.has_value());
  if (!player.has_value()) {
    return;
  }
  m_mediaTitle->setText(player->title);
  m_mediaArtist->setText(joinedArtists(player->artists));
  m_playButton->setGlyph(player->playbackStatus == "Playing" ? "player-pause" : "player-play");
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
  (void)renderer;
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
  syncMedia();
  syncPhone();
}
