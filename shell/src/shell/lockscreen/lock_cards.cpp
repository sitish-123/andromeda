#include "shell/lockscreen/lock_cards.h"

#include "dbus/link/link_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "shell/phone/phone_state.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>

namespace {

  // The lock screen is always the dark scheme (rulebook 9.15): white labels, dark regular glass.
  constexpr Color kLabel1 = rgba(1.0F, 1.0F, 1.0F, 0.96F);
  constexpr Color kLabel2 = rgba(1.0F, 1.0F, 1.0F, 0.70F);
  constexpr Color kRegular = rgba(28.0F / 255.0F, 28.0F / 255.0F, 32.0F / 255.0F, 0.40F);
  constexpr Color kStroke = rgba(1.0F, 1.0F, 1.0F, 0.14F);
  constexpr Color kFill1 = rgba(1.0F, 1.0F, 1.0F, 15.0F / 255.0F);
  constexpr Color kFill2 = rgba(1.0F, 1.0F, 1.0F, 26.0F / 255.0F);
  constexpr Color kTextShadow = rgba(0.0F, 0.0F, 0.0F, 0.30F);
  constexpr std::string_view kPhonePlayerPrefix = "org.mpris.MediaPlayer2.andromeda_link_";
  constexpr float kCardH = 64.0F;
  constexpr float kCardGap = 8.0F;

  float snap(float v) { return std::round(v); }

  RectNode* addRect(Node& parent) {
    auto rect = std::make_unique<RectNode>();
    rect->setHitTestVisible(false);
    return static_cast<RectNode*>(parent.addChild(std::move(rect)));
  }

  Label* addLabel(Node& parent, FontWeight weight, bool shadow = false) {
    auto label = std::make_unique<Label>();
    label->setFontWeight(weight);
    label->setEllipsize(TextEllipsize::End);
    if (shadow) {
      label->setShadow(kTextShadow, 0.0F, 1.0F);
    }
    return static_cast<Label*>(parent.addChild(std::move(label)));
  }

  Glyph* addGlyph(Node& parent) { return static_cast<Glyph*>(parent.addChild(std::make_unique<Glyph>())); }

  void setText(Renderer& renderer, Label* label, const std::string& text, float size, float line, Color color, float maxW) {
    label->setVisible(!text.empty());
    label->setFontSize(size);
    label->setLineHeight(line);
    label->setColor(color);
    (void)label->setText(text);
    label->setMaxWidth(std::max(0.0F, maxW));
    label->measure(renderer);
  }

  void glassCard(RectNode* bg, const AuthRect& rect, float s) {
    bg->setVisible(true);
    bg->setPosition(rect.x, rect.y);
    bg->setSize(rect.w, rect.h);
    bg->setStyle(RoundedRectStyle{.fill = kRegular, .border = kStroke, .radius = 16.0F * s, .borderWidth = 0.5F * s});
  }

  // "now" within a minute, else the local hour and minute.
  std::string timeLabel(const std::optional<WallTimePoint>& when) {
    if (!when.has_value()) {
      return {};
    }
    const auto now = WallClock::now();
    if (now - *when < std::chrono::minutes(1)) {
      return i18n::tr("lockscreen.cards.now");
    }
    const std::time_t t = WallClock::to_time_t(*when);
    std::tm tm{};
    localtime_r(&t, &tm);
    return std::format("{}:{:02}", tm.tm_hour, tm.tm_min);
  }

} // namespace

void LockCards::build(Node& parent) {
  m_group = parent.addChild(std::make_unique<Node>());
  for (Button& button : m_buttons) {
    button.bg = addRect(*m_group);
    button.glyph = addGlyph(*m_group);
  }
}

void LockCards::setData(Data data) { m_data = std::move(data); }

int LockCards::glanceCardCount() const {
  return (m_data.media.has_value() ? 1 : 0) + static_cast<int>(m_data.groups.size()) + (m_data.phone.has_value() ? 1 : 0);
}

std::vector<std::string> LockCards::chipTexts() const {
  std::vector<std::string> chips;
  if (m_data.phone.has_value()) {
    chips.push_back(std::format("{} · {}%", m_data.phone->name, m_data.phone->battery));
  }
  long count = 0;
  for (const LockNoticeGroup& group : m_data.groups) {
    count += group.count;
  }
  if (m_data.phone.has_value()) {
    count += m_data.phone->notices;
  }
  if (count > 0) {
    chips.push_back(i18n::trp("lockscreen.cards.notifications", count));
  }
  if (!m_data.charging.empty()) {
    chips.push_back(m_data.charging);
  }
  return chips;
}

std::vector<std::pair<std::string, std::string>> LockCards::widgets() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (m_data.phone.has_value()) {
    out.emplace_back(std::format("{}%", m_data.phone->battery), m_data.phone->name);
  }
  long count = 0;
  for (const LockNoticeGroup& group : m_data.groups) {
    count += group.count;
  }
  if (count > 0) {
    out.emplace_back(std::to_string(count), i18n::tr("lockscreen.cards.notifications-label"));
  }
  return out;
}

void LockCards::fillPlacementInput(AuthPlacementInput& in) const {
  in.media = m_data.media.has_value();
  in.chips = !chipTexts().empty();
  in.cards = in.layout == AuthLayout::Glance ? glanceCardCount()
      : in.layout == AuthLayout::Split       ? static_cast<int>(widgets().size())
                                             : 0;
}

void LockCards::hideAll() {
  for (Card& card : m_cards) {
    for (Node* node : {static_cast<Node*>(card.bg), static_cast<Node*>(card.image), static_cast<Node*>(card.tile),
                       static_cast<Node*>(card.glyph), static_cast<Node*>(card.title), static_cast<Node*>(card.titleRest),
                       static_cast<Node*>(card.body), static_cast<Node*>(card.trailing)}) {
      node->setVisible(false);
    }
  }
  for (Chip& chip : m_chips) {
    chip.bg->setVisible(false);
    chip.label->setVisible(false);
  }
  for (Widget& widget : m_widgets) {
    widget.bg->setVisible(false);
    widget.value->setVisible(false);
    widget.label->setVisible(false);
  }
  for (Button& button : m_buttons) {
    button.bg->setVisible(false);
    button.glyph->setVisible(false);
    button.rect = {};
  }
  m_buttonsShown = false;
}

void LockCards::layout(Renderer& renderer, const AuthPlacement& place, float scale) {
  if (m_group == nullptr) {
    return;
  }
  const float s = std::max(0.1F, scale);
  hideAll();

  std::vector<AuthRect> cardRects;
  if (!place.media.empty()) {
    cardRects.push_back(place.media);
  }
  if (place.layout == AuthLayout::Glance && !place.cards.empty()) {
    for (int i = 0; i < glanceCardCount(); ++i) {
      cardRects.push_back(
          {place.cards.x, place.cards.y + static_cast<float>(i) * snap((kCardH + kCardGap) * s), place.cards.w, snap(kCardH * s)}
      );
    }
  }
  while (m_cards.size() < cardRects.size()) {
    Card card;
    card.bg = addRect(*m_group);
    auto image = std::make_unique<Image>();
    card.image = static_cast<Image*>(m_group->addChild(std::move(image)));
    card.tile = addRect(*m_group);
    card.glyph = addGlyph(*m_group);
    card.title = addLabel(*m_group, FontWeight::SemiBold);
    card.titleRest = addLabel(*m_group, FontWeight::Normal);
    card.body = addLabel(*m_group, FontWeight::Normal);
    card.trailing = addLabel(*m_group, FontWeight::Normal);
    m_cards.push_back(card);
  }

  std::size_t next = 0;
  if (!place.media.empty() && m_data.media.has_value()) {
    layoutMediaCard(renderer, m_cards[next], place.media, s);
    ++next;
  }
  if (place.layout == AuthLayout::Glance && !place.cards.empty()) {
    std::size_t row = 0;
    const auto rectAt = [&](std::size_t i) { return cardRects[(place.media.empty() ? 0 : 1) + i]; };
    if (m_data.media.has_value()) {
      layoutMediaCard(renderer, m_cards[next++], rectAt(row++), s);
    }
    for (const LockNoticeGroup& group : m_data.groups) {
      layoutNoticeCard(renderer, m_cards[next++], group, rectAt(row++), s);
    }
    if (m_data.phone.has_value()) {
      layoutPhoneCard(renderer, m_cards[next++], rectAt(row++), s);
    }
  }

  // Chips row: h 24, r 12, p 0 / 8, fill_1, footnote label_2, g 8, centred in its slot.
  if (!place.chips.empty()) {
    const std::vector<std::string> texts = chipTexts();
    while (m_chips.size() < texts.size()) {
      m_chips.push_back({addRect(*m_group), addLabel(*m_group, FontWeight::Normal)});
    }
    float total = 0.0F;
    for (std::size_t i = 0; i < texts.size(); ++i) {
      setText(renderer, m_chips[i].label, texts[i], Tokens::typeFootnote.size * s, Tokens::typeFootnote.line * s, kLabel2,
              place.chips.w);
      total += m_chips[i].label->width() + 16.0F * s;
    }
    total += 8.0F * s * static_cast<float>(texts.empty() ? 0 : texts.size() - 1);
    float x = snap(place.chips.x + (place.chips.w - total) * 0.5F);
    for (std::size_t i = 0; i < texts.size(); ++i) {
      const float w = snap(m_chips[i].label->width() + 16.0F * s);
      m_chips[i].bg->setVisible(true);
      m_chips[i].bg->setPosition(x, place.chips.y);
      m_chips[i].bg->setSize(w, place.chips.h);
      m_chips[i].bg->setStyle(RoundedRectStyle{.fill = kFill1, .radius = place.chips.h * 0.5F});
      m_chips[i].label->setPosition(x + 8.0F * s, snap(place.chips.y + (place.chips.h - m_chips[i].label->height()) * 0.5F));
      x += w + 8.0F * s;
    }
  }

  // Split widget row: 112 x 56, p 12; value title-3, label footnote label_2.
  if (place.layout == AuthLayout::Split && !place.cards.empty()) {
    const auto items = widgets();
    while (m_widgets.size() < items.size()) {
      m_widgets.push_back(
          {addRect(*m_group), addLabel(*m_group, FontWeight::SemiBold), addLabel(*m_group, FontWeight::Normal)}
      );
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
      const AuthRect rect{
          place.cards.x + static_cast<float>(i) * snap((112.0F + kCardGap) * s), place.cards.y, snap(112.0F * s),
          place.cards.h
      };
      Widget& widget = m_widgets[i];
      glassCard(widget.bg, rect, s);
      const float innerW = rect.w - 24.0F * s;
      setText(renderer, widget.value, items[i].first, Tokens::typeTitle3.size * s, Tokens::typeTitle3.line * s, kLabel1, innerW);
      setText(renderer, widget.label, items[i].second, Tokens::typeFootnote.size * s, Tokens::typeFootnote.line * s, kLabel2, innerW);
      const float textH = Tokens::typeTitle3.line * s + Tokens::typeFootnote.line * s;
      const float top = snap(rect.y + (rect.h - textH) * 0.5F);
      widget.value->setPosition(rect.x + 12.0F * s, top);
      widget.label->setPosition(rect.x + 12.0F * s, top + Tokens::typeTitle3.line * s);
    }
  }
}

// Media card: art 40 r 8; title body 600 + footnote label_2 "artist · app"; controls 32 circles g 4, the playing
// control on fill_2.
void LockCards::layoutMediaCard(Renderer& renderer, Card& card, const AuthRect& rect, float s) {
  const Media& media = *m_data.media;
  glassCard(card.bg, rect, s);
  const float pad = 12.0F * s;
  const float art = 40.0F * s;
  const float artY = snap(rect.y + (rect.h - art) * 0.5F);
  const bool hasArt = !media.artPath.empty();
  if (hasArt && media.artPath != card.imagePath) {
    card.imagePath = card.image->setSourceFile(renderer, media.artPath, static_cast<int>(art), true, true) ? media.artPath
                                                                                                           : std::string{};
  }
  const bool showArt = hasArt && card.imagePath == media.artPath;
  card.image->setVisible(showArt);
  card.tile->setVisible(!showArt);
  card.glyph->setVisible(!showArt);
  if (showArt) {
    card.image->setPosition(rect.x + pad, artY);
    card.image->setSize(art, art);
    card.image->setRadius(8.0F * s);
  } else {
    card.tile->setPosition(rect.x + pad, artY);
    card.tile->setSize(art, art);
    card.tile->setStyle(RoundedRectStyle{.fill = kFill2, .radius = 8.0F * s});
    card.glyph->setGlyph("music");
    card.glyph->setGlyphSize(20.0F * s);
    card.glyph->setColor(kLabel2);
    card.glyph->measure(renderer);
    card.glyph->setPosition(
        snap(rect.x + pad + (art - card.glyph->width()) * 0.5F), snap(artY + (art - card.glyph->height()) * 0.5F)
    );
  }

  const float control = 32.0F * s;
  const float controlsW = control * 3.0F + 4.0F * s * 2.0F;
  const float controlY = snap(rect.y + (rect.h - control) * 0.5F);
  float cx = rect.right() - pad - controlsW;
  const std::array<const char*, 3> glyphs{"media-prev", media.playing ? "media-pause" : "media-play", "media-next"};
  for (std::size_t i = 0; i < m_buttons.size(); ++i) {
    Button& button = m_buttons[i];
    button.rect = {snap(cx), controlY, control, control};
    button.bg->setVisible(i == 1 && media.playing);
    button.bg->setPosition(button.rect.x, button.rect.y);
    button.bg->setSize(control, control);
    button.bg->setStyle(RoundedRectStyle{.fill = kFill2, .radius = control * 0.5F});
    button.glyph->setVisible(true);
    button.glyph->setGlyph(glyphs[i]);
    button.glyph->setGlyphSize(16.0F * s);
    button.glyph->setColor(kLabel1);
    button.glyph->measure(renderer);
    button.glyph->setPosition(
        snap(button.rect.x + (control - button.glyph->width()) * 0.5F),
        snap(button.rect.y + (control - button.glyph->height()) * 0.5F)
    );
    cx += control + 4.0F * s;
  }
  m_buttonsShown = true;

  const float textX = rect.x + pad + art + 12.0F * s;
  const float textW = rect.right() - pad - controlsW - 12.0F * s - textX;
  setText(renderer, card.title, media.title, Tokens::typeBody.size * s, Tokens::typeBody.line * s, kLabel1, textW);
  setText(renderer, card.body, media.subtitle, Tokens::typeFootnote.size * s, Tokens::typeFootnote.line * s, kLabel2, textW);
  const float textH = Tokens::typeBody.line * s + (media.subtitle.empty() ? 0.0F : Tokens::typeFootnote.line * s);
  const float top = snap(rect.y + (rect.h - textH) * 0.5F);
  card.title->setPosition(textX, top);
  card.body->setPosition(textX, top + Tokens::typeBody.line * s);
}

// Notification group: app icon 24 r 6; "**App** · N notifications"; footnote "Unlock to read"; trailing time.
void LockCards::layoutNoticeCard(
    Renderer& renderer, Card& card, const LockNoticeGroup& group, const AuthRect& rect, float s
) {
  glassCard(card.bg, rect, s);
  const float pad = 12.0F * s;
  const float icon = 24.0F * s;
  const float iconY = snap(rect.y + (rect.h - icon) * 0.5F);
  const bool file = !group.icon.empty() && group.icon.front() == '/';
  if (file && group.icon != card.imagePath) {
    card.imagePath =
        card.image->setSourceFile(renderer, group.icon, static_cast<int>(icon), true, true) ? group.icon : std::string{};
  }
  const bool showIcon = file && card.imagePath == group.icon;
  card.image->setVisible(showIcon);
  card.tile->setVisible(!showIcon);
  card.glyph->setVisible(!showIcon);
  if (showIcon) {
    card.image->setPosition(rect.x + pad, iconY);
    card.image->setSize(icon, icon);
    card.image->setRadius(6.0F * s);
  } else {
    card.tile->setPosition(rect.x + pad, iconY);
    card.tile->setSize(icon, icon);
    card.tile->setStyle(RoundedRectStyle{.fill = kFill2, .radius = 6.0F * s});
    card.glyph->setGlyph("bell");
    card.glyph->setGlyphSize(14.0F * s);
    card.glyph->setColor(kLabel1);
    card.glyph->measure(renderer);
    card.glyph->setPosition(
        snap(rect.x + pad + (icon - card.glyph->width()) * 0.5F), snap(iconY + (icon - card.glyph->height()) * 0.5F)
    );
  }

  const float textX = rect.x + pad + icon + 12.0F * s;
  setText(renderer, card.trailing, timeLabel(group.newest), Tokens::typeFootnote.size * s, Tokens::typeFootnote.line * s,
          kLabel2, 80.0F * s);
  const float trailingW = card.trailing->visible() ? card.trailing->width() + 12.0F * s : 0.0F;
  const float textW = rect.right() - pad - trailingW - textX;
  setText(renderer, card.title, group.app, Tokens::typeBody.size * s, Tokens::typeBody.line * s, kLabel1, textW * 0.6F);
  setText(renderer, card.titleRest, " · " + i18n::trp("lockscreen.cards.notifications", group.count),
          Tokens::typeBody.size * s, Tokens::typeBody.line * s, kLabel1, textW - card.title->width());
  setText(renderer, card.body, i18n::tr("lockscreen.cards.unlock-to-read"), Tokens::typeFootnote.size * s,
          Tokens::typeFootnote.line * s, kLabel2, textW);
  const float textH = Tokens::typeBody.line * s + Tokens::typeFootnote.line * s;
  const float top = snap(rect.y + (rect.h - textH) * 0.5F);
  card.title->setPosition(textX, top);
  card.titleRest->setPosition(textX + card.title->width(), top);
  card.body->setPosition(textX, top + Tokens::typeBody.line * s);
  card.trailing->setPosition(snap(rect.right() - pad - card.trailing->width()), top);
}

// Phone card (Glance): phone symbol on a 24 tile; "Name · 82 %"; footnote "Phone".
void LockCards::layoutPhoneCard(Renderer& renderer, Card& card, const AuthRect& rect, float s) {
  const Phone& phone = *m_data.phone;
  glassCard(card.bg, rect, s);
  const float pad = 12.0F * s;
  const float icon = 24.0F * s;
  const float iconY = snap(rect.y + (rect.h - icon) * 0.5F);
  card.image->setVisible(false);
  card.tile->setVisible(true);
  card.tile->setPosition(rect.x + pad, iconY);
  card.tile->setSize(icon, icon);
  card.tile->setStyle(RoundedRectStyle{.fill = kFill2, .radius = 6.0F * s});
  card.glyph->setVisible(true);
  card.glyph->setGlyph(phone.glyph.empty() ? "device-mobile" : phone.glyph);
  card.glyph->setGlyphSize(14.0F * s);
  card.glyph->setColor(kLabel1);
  card.glyph->measure(renderer);
  card.glyph->setPosition(
      snap(rect.x + pad + (icon - card.glyph->width()) * 0.5F), snap(iconY + (icon - card.glyph->height()) * 0.5F)
  );
  const float textX = rect.x + pad + icon + 12.0F * s;
  const float textW = rect.right() - pad - textX;
  setText(renderer, card.title, std::format("{} · {} %", phone.name, phone.battery), Tokens::typeBody.size * s,
          Tokens::typeBody.line * s, kLabel1, textW);
  setText(renderer, card.body, i18n::tr("lockscreen.cards.phone"), Tokens::typeFootnote.size * s,
          Tokens::typeFootnote.line * s, kLabel2, textW);
  const float textH = Tokens::typeBody.line * s + Tokens::typeFootnote.line * s;
  const float top = snap(rect.y + (rect.h - textH) * 0.5F);
  card.title->setPosition(textX, top);
  card.body->setPosition(textX, top + Tokens::typeBody.line * s);
}

LockCards::Control LockCards::hitTest(float x, float y) const {
  if (!m_buttonsShown) {
    return Control::None;
  }
  constexpr std::array<Control, 3> kControls{Control::Previous, Control::PlayPause, Control::Next};
  for (std::size_t i = 0; i < m_buttons.size(); ++i) {
    const AuthRect& r = m_buttons[i].rect;
    if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) {
      return kControls[i];
    }
  }
  return Control::None;
}

LockCards::Data collectLockCards(
    const MprisService* mpris, const LinkService* link, const NotificationManager* notifications
) {
  LockCards::Data data;
  if (mpris != nullptr) {
    if (const auto active = mpris->activePlayer(); active.has_value() && !active->title.empty()) {
      const std::string artists = mpris::joinArtists(active->artists);
      const std::string app = active->identity;
      data.media = LockCards::Media{
          .busName = active->busName,
          .title = active->title,
          .subtitle = artists.empty() ? app : app.empty() ? artists : artists + " · " + app,
          .artUrl = mpris::effectiveArtUrl(*active),
          .playing = active->playbackStatus == "Playing",
      };
    }
  }
  if (link != nullptr) {
    if (const LinkDevice* phone = phone::primary(*link); phone != nullptr && phone->connected) {
      const LinkStatus* status = link->status(phone->id);
      data.phone = LockCards::Phone{
          .name = phone->name,
          .glyph = phone::glyph(*phone, status),
          .battery = status != nullptr ? status->battery : 0,
          .notices = link->granted(phone->id, "notifications") ? link->notificationCount(phone->id) : 0,
      };
      // The phone's own player shows as the phone card's line, not as the desktop media card.
      if (data.media.has_value() && data.media->busName == std::string(kPhonePlayerPrefix) + phone->id) {
        data.media.reset();
      }
    }
  }
  if (notifications != nullptr) {
    data.groups = groupUnseenNotices(notifications->history());
  }
  return data;
}
