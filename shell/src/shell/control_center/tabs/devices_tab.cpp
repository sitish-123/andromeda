#include "shell/control_center/tabs/devices_tab.h"

#include "dbus/link/link_service.h"
#include "dbus/link/quickshare_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <qrencode.h>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kQrSize = 176.0F;
  // Modules of white border the QR spec requires around the symbol.
  constexpr int kQrQuietZone = 4;

  // Black on white whatever the theme, because phone scanners expect dark modules on a light field. The texture holds
  // two texels per displayed pixel on each axis, so module edges stay sharp at the returned display size.
  std::unique_ptr<Image> makeQrImage(Renderer& renderer, const std::string& text, float targetSize, float radius) {
    const std::unique_ptr<QRcode, decltype(&QRcode_free)> qr(
        QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1), &QRcode_free
    );
    if (qr == nullptr) {
      return nullptr;
    }
    const int modules = qr->width + 2 * kQrQuietZone;
    const int pointsPerModule = std::max(1, static_cast<int>(std::lround(targetSize / static_cast<float>(modules))));
    const int texelsPerModule = pointsPerModule * 2;
    const int side = modules * texelsPerModule;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4, 0xFF);
    for (int my = 0; my < qr->width; ++my) {
      for (int mx = 0; mx < qr->width; ++mx) {
        if ((qr->data[(my * qr->width) + mx] & 1) == 0) {
          continue;
        }
        for (int y = 0; y < texelsPerModule; ++y) {
          const int row = ((my + kQrQuietZone) * texelsPerModule) + y;
          const auto start = static_cast<std::size_t>((row * side) + ((mx + kQrQuietZone) * texelsPerModule)) * 4;
          for (std::size_t i = 0; i < static_cast<std::size_t>(texelsPerModule); ++i) {
            rgba[start + (i * 4)] = 0;
            rgba[start + (i * 4) + 1] = 0;
            rgba[start + (i * 4) + 2] = 0;
          }
        }
      }
    }
    const auto displaySize = static_cast<float>(modules * pointsPerModule);
    auto image = ui::image({.fit = ImageFit::Stretch, .radius = radius, .width = displaySize, .height = displaySize});
    if (!image->setSourceRaw(renderer, rgba.data(), rgba.size(), side, side, side * 4, PixmapFormat::RGBA)) {
      return nullptr;
    }
    return image;
  }

  std::string remainingText(const LinkPairing& pairing) {
    const auto left = std::chrono::ceil<std::chrono::seconds>(pairing.deadline - std::chrono::steady_clock::now());
    const auto seconds = std::max<std::int64_t>(0, left.count());
    return i18n::tr("control-center.devices.expires", "time", std::format("{}:{:02}", seconds / 60, seconds % 60));
  }

  std::unique_ptr<Flex> makeCard(float scale, float opacity) {
    return ui::column({
        .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
    });
  }

  std::unique_ptr<Label> makeCaption(std::string text, float scale, ColorRole role = ColorRole::OnSurfaceVariant) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(role),
    });
  }

} // namespace

DevicesTab::DevicesTab(LinkService* link, QuickShareService* quickShare) : m_link(link), m_quickShare(quickShare) {}

std::unique_ptr<Flex> DevicesTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });
  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);
  tab->addChild(std::move(listScroll));
  return tab;
}

void DevicesTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  rebuild(renderer);
  m_rootLayout->layout(renderer);
}

void DevicesTab::doUpdate(Renderer& renderer) {
  rebuild(renderer);
  syncCountdown();
}

void DevicesTab::setActive(bool active) {
  m_active = active;
  syncCountdown();
}

void DevicesTab::onClose() {
  m_countdownTimer.stop();
  m_rootLayout = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_countdown = nullptr;
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0F;
}

void DevicesTab::syncCountdown() {
  const bool showing = m_active && m_countdown != nullptr && m_link != nullptr && m_link->pairing().has_value();
  if (!showing) {
    m_countdownTimer.stop();
    return;
  }
  m_countdown->setText(remainingText(*m_link->pairing()));
  if (!m_countdownTimer.active()) {
    // The remaining time has no change signal; this ticks only while the code is on screen.
    m_countdownTimer.startRepeating(std::chrono::seconds(1), []() { PanelManager::instance().refresh(); });
  }
}

std::string DevicesTab::structureKey() const {
  if (m_link == nullptr) {
    return {};
  }
  std::string key;
  if (const auto& pairing = m_link->pairing()) {
    key += "open " + pairing->uri;
  } else if (const auto& outcome = m_link->outcome()) {
    key += (outcome->paired ? "paired " : "failed ") + outcome->detail;
  }
  key.push_back('\n');
  for (const auto& device : m_link->devices()) {
    key += device.id + (device.connected ? " 1 " : " 0 ") + (m_link->phoneRinging(device.id) ? "r " : "- ") +
        device.name + "\n";
  }
  if (m_quickShare != nullptr && m_quickShare->available()) {
    key += m_quickShare->visible() ? "quick-share 1 " : "quick-share 0 ";
    key += m_quickShare->name();
  }
  return key;
}

void DevicesTab::rebuild(Renderer& renderer) {
  if (m_list == nullptr || m_listScroll == nullptr || m_link == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0F) {
    return;
  }
  std::string nextKey = structureKey();
  if (listWidth == m_lastListWidth && nextKey == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = std::move(nextKey);

  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  m_countdown = nullptr;
  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  auto pairCard = makeCard(scale, opacity);
  auto header = makeCardHeaderRow(i18n::tr("control-center.devices.pair-title"), scale);
  if (const auto& pairing = m_link->pairing()) {
    header->addChild(
        ui::button({
            .text = i18n::tr("control-center.devices.cancel"),
            .variant = ButtonVariant::Ghost,
            .onClick = [this]() { m_link->cancelPairing(); },
        })
    );
    pairCard->addChild(std::move(header));

    auto codeColumn = ui::column(
        {.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .flexGrow = 1.0F},
        makeCaption(i18n::tr("control-center.devices.scan"), scale),
        ui::label({
            .text = pairing->code.substr(0, 3) + " " + pairing->code.substr(3),
            .fontSize = Style::fontSizeHeader * 1.6F * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    );
    auto countdown = makeCaption(remainingText(*pairing), scale);
    m_countdown = countdown.get();
    codeColumn->addChild(std::move(countdown));

    auto body = ui::row({.align = FlexAlign::Center, .gap = Style::spaceLg * scale});
    if (auto qr = makeQrImage(renderer, pairing->uri, kQrSize * scale, Style::scaledRadiusMd(scale))) {
      body->addChild(std::move(qr));
    }
    body->addChild(std::move(codeColumn));
    pairCard->addChild(std::move(body));
  } else {
    header->addChild(
        ui::button({
            .text = i18n::tr("control-center.devices.pair"),
            .glyph = "qrcode",
            .variant = ButtonVariant::Default,
            .onClick = [this]() { m_link->startPairing(); },
        })
    );
    pairCard->addChild(std::move(header));
    if (const auto& outcome = m_link->outcome()) {
      pairCard->addChild(
          outcome->paired
              ? makeCaption(
                    i18n::tr("control-center.devices.paired-with", "device", outcome->detail), scale, ColorRole::Primary
                )
              : makeCaption(
                    i18n::tr("control-center.devices.pairing-failed", "reason", outcome->detail), scale,
                    ColorRole::Error
                )
      );
    } else {
      pairCard->addChild(makeCaption(i18n::tr("control-center.devices.pair-hint"), scale));
    }
  }
  m_list->addChild(std::move(pairCard));

  auto devicesCard = makeCard(scale, opacity);
  devicesCard->addChild(makeCardHeaderRow(i18n::tr("control-center.devices.paired"), scale));
  if (m_link->devices().empty()) {
    devicesCard->addChild(makeCaption(i18n::tr("control-center.devices.none"), scale));
  }
  for (const auto& device : m_link->devices()) {
    auto status = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::box({
            .fill = colorSpecFromRole(device.connected ? ColorRole::Primary : ColorRole::Outline),
            .radius = 4.0F * scale,
            .width = 8.0F * scale,
            .height = 8.0F * scale,
        }),
        makeCaption(
            i18n::tr(device.connected ? "control-center.devices.connected" : "control-center.devices.not-connected"),
            scale
        )
    );
    auto row = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingV = Style::spaceSm * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::Surface),
         .radius = Style::scaledRadiusMd(scale),
         .minHeight = Style::controlHeightLg * scale},
        ui::glyph({
            .glyph = "device-mobile",
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::column(
            {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
            ui::label({
                .text = device.name,
                .fontSize = Style::fontSizeBody * scale,
                .fontWeight = device.connected ? FontWeight::Bold : FontWeight::Normal,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            }),
            std::move(status)
        )
    );
    if (device.connected) {
      const bool ringing = m_link->phoneRinging(device.id);
      row->addChild(
          ui::button({
              .glyph = ringing ? "bell-off" : "bell-ringing",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ringing ? ButtonVariant::Default : ButtonVariant::Ghost,
              .tooltip = i18n::tr(ringing ? "control-center.devices.stop-ring" : "control-center.devices.ring"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id, ringing]() { m_link->ring(id, !ringing); },
          })
      );
      row->addChild(
          ui::button({
              .glyph = "clipboard",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.devices.send-clipboard"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id]() { m_link->shareClipboard(id); },
          })
      );
    }
    row->addChild(
        ui::button({
            .glyph = "unlink",
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.devices.unpair"),
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [this, id = device.id]() { m_link->unpair(id); },
        })
    );
    devicesCard->addChild(std::move(row));
  }
  m_list->addChild(std::move(devicesCard));

  if (m_quickShare != nullptr && m_quickShare->available()) {
    auto quickShareCard = makeCard(scale, opacity);
    auto quickShareHeader = makeCardHeaderRow(i18n::tr("control-center.devices.quick-share"), scale);
    quickShareHeader->addChild(ui::toggle({
        .checked = m_quickShare->visible(),
        .scale = scale,
        .onChange = [this](bool visible) { m_quickShare->setVisible(visible); },
    }));
    quickShareCard->addChild(std::move(quickShareHeader));
    quickShareCard->addChild(makeCaption(
        m_quickShare->visible() ? i18n::tr("control-center.devices.quick-share-on", "name", m_quickShare->name())
                                : i18n::tr("control-center.devices.quick-share-off"),
        scale
    ));
    m_list->addChild(std::move(quickShareCard));
  }
  m_list->layout(renderer);
}
