#include "shell/lockscreen/lock_phone_row.h"

#include "dbus/link/link_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/phone/phone_state.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

  // The bus name andromeda-linkd exports a phone's player under, followed by the device id.
  constexpr std::string_view kPhonePlayerPrefix = "org.mpris.MediaPlayer2.andromeda_link_";
  constexpr float kMargin = 48.0F;
  constexpr float kMaxPlayerWidth = 320.0F;

  std::unique_ptr<Glyph> makeGlyph(Glyph** out, std::string glyph) {
    return ui::glyph({
        .out = out,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeTitle,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
  }

  std::unique_ptr<Label> makeText(Label** out) {
    return ui::label({
        .out = out,
        .fontSize = Style::fontSizeBody,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = 1,
    });
  }

} // namespace

std::unique_ptr<Flex> LockPhoneRow::create() {
  auto row = ui::row({
      .out = &m_row,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm,
      .paddingV = Style::spaceSm,
      .paddingH = Style::spaceMd,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .radius = Style::scaledRadiusLg(1.0F),
      .visible = false,
      .configure = [](Flex& flex) { flex.setZIndex(3); },
  });
  row->addChild(makeGlyph(&m_battery, "device-mobile"));
  row->addChild(makeText(&m_phone));
  row->addChild(makeGlyph(&m_playerGlyph, "music"));
  row->addChild(makeText(&m_player));
  row->addChild(makeGlyph(&m_countGlyph, "bell"));
  row->addChild(makeText(&m_count));
  m_player->setMaxWidth(kMaxPlayerWidth);
  m_player->setEllipsize(TextEllipsize::End);
  return row;
}

void LockPhoneRow::setServices(const LinkService* link) { m_link = link; }

void LockPhoneRow::sync(const MprisService* mpris) {
  if (m_row == nullptr) {
    return;
  }
  const LinkDevice* phone = m_link != nullptr ? phone::primary(*m_link) : nullptr;
  m_hasPhone = phone != nullptr && phone->connected;
  if (!m_hasPhone) {
    return;
  }
  const LinkStatus* status = m_link->status(phone->id);
  m_battery->setGlyph(phone::glyph(*phone, status));
  (void)m_phone->setText(status != nullptr ? std::format("{} · {}%", phone->name, status->battery) : phone->name);

  std::string playing;
  if (mpris != nullptr) {
    const auto& players = mpris->players();
    if (const auto it = players.find(std::string(kPhonePlayerPrefix) + phone->id);
        it != players.end() && !it->second.title.empty()) {
      const std::string artists = mpris::joinArtists(it->second.artists);
      playing = artists.empty() ? it->second.title : it->second.title + " — " + artists;
    }
  }
  (void)m_player->setText(playing);
  m_playerGlyph->setVisible(!playing.empty());
  m_player->setVisible(!playing.empty());

  // Only counted, as link-session shows a locked desktop nothing more by default.
  const std::uint32_t count = m_link->granted(phone->id, "notifications") ? m_link->notificationCount(phone->id) : 0;
  (void)m_count->setText(i18n::trp("lockscreen.phone-notifications", count));
  m_countGlyph->setVisible(count > 0);
  m_count->setVisible(count > 0);
}

void LockPhoneRow::layout(Renderer& renderer, float screenWidth, float screenHeight, bool shown) {
  if (m_row == nullptr) {
    return;
  }
  m_row->setVisible(shown && m_hasPhone);
  if (!m_row->visible()) {
    return;
  }
  LayoutConstraints constraints;
  constraints.setMaxWidth(std::max(0.0F, screenWidth - 2.0F * kMargin));
  const LayoutSize size = m_row->measure(renderer, constraints);
  m_row->arrange(
      renderer, LayoutRect{kMargin, std::round(screenHeight - size.height - kMargin), size.width, size.height}
  );
}
