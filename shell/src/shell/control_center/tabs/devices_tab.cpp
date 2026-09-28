#include "shell/control_center/tabs/devices_tab.h"

#include "dbus/link/link_service.h"
#include "dbus/link/quickshare_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <qrencode.h>
#include <string_view>
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

  struct Feature {
    std::string_view name;
    std::string_view label;
  };
  constexpr std::array kFeatures{
      Feature{.name = "clipboard", .label = "control-center.devices.grant-clipboard"},
      Feature{.name = "files", .label = "control-center.devices.grant-files"},
      Feature{.name = "notifications", .label = "control-center.devices.grant-notifications"},
      Feature{.name = "media", .label = "control-center.devices.grant-media"},
      Feature{.name = "ring", .label = "control-center.devices.grant-ring"},
      Feature{.name = "calls", .label = "control-center.devices.grant-calls"},
  };

  std::string statusText(const LinkDevice& device, const LinkStatus* status) {
    if (!device.connected) {
      return i18n::tr("control-center.devices.not-connected");
    }
    if (status == nullptr) {
      return i18n::tr("control-center.devices.connected");
    }
    return i18n::tr(
        status->charging ? "control-center.devices.status-charging" : "control-center.devices.status", "battery",
        std::to_string(status->battery), "network", i18n::tr("bar.widgets.phone.network-" + status->network)
    );
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
  if (!m_pendingSend.empty() && m_quickShare != nullptr) {
    m_quickShare->setDiscovering(false);
  }
  m_pendingSend.clear();
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
  key += m_link->localSendVisible() ? "\nlocalsend on\n" : "\nlocalsend off\n";
  for (const auto& peer : m_link->nearby()) {
    key += peer.id + " " + peer.alias + "\n";
  }
  for (const auto& device : m_link->devices()) {
    key +=
        device.id + (device.connected ? " 1 " : " 0 ") + (m_link->phoneRinging(device.id) ? "r " : "- ") + device.name;
    if (const LinkStatus* status = m_link->status(device.id)) {
      key += std::format(" {} {} {}", status->battery, status->charging, status->network);
    }
    for (const auto feature : kFeatures) {
      key += m_link->granted(device.id, feature.name) ? " +" : " -";
    }
    key += std::ranges::contains(m_link->autoAccept(), device.id) ? " auto\n" : "\n";
  }
  key += m_pendingSend + "\n";
  if (m_quickShare != nullptr && m_quickShare->available()) {
    key += m_quickShare->visible() ? "quick-share 1 " : "quick-share 0 ";
    key += m_quickShare->name() + "\n";
    for (const auto& [id, name] : m_quickShare->nearby()) {
      key += id + " " + name + "\n";
    }
  }
  return key;
}

// Quick Share and LocalSend in one card: each transport's visibility switch, then one "Send a file" whose chooser lists
// the peers of both, labeled by transport.
std::unique_ptr<Flex> DevicesTab::makeNearby(float scale, float opacity) {
  auto card = makeCard(scale, opacity);
  card->addChild(makeCardHeaderRow(i18n::tr("control-center.devices.nearby"), scale));
  const auto addSwitch = [&](std::string label, bool checked, std::function<void(bool)> onChange) {
    card->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::label({
                .text = std::move(label),
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .flexGrow = 1.0F,
            }),
            ui::toggle({
                .checkedImmediate = checked,
                .toggleSize = ToggleSize::Small,
                .scale = scale,
                .onChange = std::move(onChange),
            })
        )
    );
  };
  const bool quickShare = m_quickShare != nullptr && m_quickShare->available();
  if (quickShare) {
    addSwitch(i18n::tr("control-center.devices.quick-share"), m_quickShare->visible(), [this](bool visible) {
      m_quickShare->setVisible(visible);
    });
    card->addChild(makeCaption(
        m_quickShare->visible() ? i18n::tr("control-center.devices.quick-share-on", "name", m_quickShare->name())
                                : i18n::tr("control-center.devices.quick-share-off"),
        scale
    ));
  }
  addSwitch(i18n::tr("control-center.devices.localsend-visible"), m_link->localSendVisible(), [this](bool on) {
    m_link->setLocalSendVisible(on);
  });

  if (m_pendingSend.empty()) {
    card->addChild(
        ui::button({
            .text = i18n::tr("quick-share.send-files"),
            .glyph = "share",
            .variant = ButtonVariant::Default,
            .onClick = [this]() {
              FileDialogOptions options;
              options.title = i18n::tr("quick-share.send-files");
              (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> path) {
                if (!path.has_value()) {
                  return;
                }
                m_pendingSend = path->string();
                if (m_quickShare != nullptr && m_quickShare->available()) {
                  m_quickShare->setDiscovering(true);
                }
                PanelManager::instance().refresh();
              });
            },
        })
    );
    return card;
  }

  auto chooser = makeCardHeaderRow(i18n::tr("quick-share.choose-device"), scale);
  chooser->addChild(
      ui::button({
          .text = i18n::tr("quick-share.cancel"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { finishPendingSend(); },
      })
  );
  card->addChild(std::move(chooser));
  card->addChild(makeCaption(std::filesystem::path(m_pendingSend).filename().string(), scale));
  const auto addPeer = [&](const std::string& name, const std::string& transport, std::function<void()> send) {
    card->addChild(
        ui::button({
            .text = name + " · " + transport,
            .glyph = "device-mobile",
            .variant = ButtonVariant::Default,
            .onClick = [this, send = std::move(send)]() {
              send();
              finishPendingSend();
            },
        })
    );
  };
  bool any = false;
  if (quickShare) {
    for (const auto& [id, name] : m_quickShare->nearby()) {
      any = true;
      addPeer(name, i18n::tr("control-center.devices.quick-share"), [this, peer = id]() {
        m_quickShare->send(peer, {m_pendingSend});
      });
    }
  }
  for (const auto& peer : m_link->nearby()) {
    any = true;
    addPeer(peer.alias, i18n::tr("control-center.devices.localsend"), [this, id = peer.id]() {
      (void)m_link->sendFiles(id, {m_pendingSend});
    });
  }
  if (!any) {
    card->addChild(makeCaption(i18n::tr("quick-share.looking"), scale));
  }
  return card;
}

void DevicesTab::finishPendingSend() {
  m_pendingSend.clear();
  if (m_quickShare != nullptr && m_quickShare->available()) {
    m_quickShare->setDiscovering(false);
  }
}

void DevicesTab::pickAndSend(const std::string& id) {
  FileDialogOptions options;
  options.mode = FileDialogMode::Open;
  options.title = i18n::tr("control-center.devices.send-files");
  (void)FileDialog::open(std::move(options), [link = m_link, id](std::optional<std::filesystem::path> path) {
    if (path.has_value()) {
      (void)link->sendFiles(id, {path->string()});
    }
  });
}

std::unique_ptr<Flex> DevicesTab::makeSettings(const std::string& id, float scale) {
  auto settings =
      ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .paddingH = Style::spaceMd * scale});
  const auto addToggle = [&](std::string label, bool checked, std::function<void(bool)> onChange) {
    settings->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
            ui::label({
                .text = std::move(label),
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0F,
            }),
            ui::toggle({
                .checkedImmediate = checked,
                .toggleSize = ToggleSize::Small,
                .scale = scale,
                .onChange = std::move(onChange),
            })
        )
    );
  };
  for (const auto feature : kFeatures) {
    addToggle(i18n::tr(feature.label), m_link->granted(id, feature.name), [this, id, name = feature.name](bool on) {
      m_link->setGrant(id, std::string(name), on);
    });
  }
  addToggle(
      i18n::tr("control-center.devices.auto-accept"), std::ranges::contains(m_link->autoAccept(), id),
      [this, id](bool on) { m_link->setAutoAccept(id, on); }
  );
  return settings;
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
        makeCaption(statusText(device, m_link->status(device.id)), scale)
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
      row->addChild(
          ui::button({
              .glyph = "send",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.devices.send-files"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id]() {
                FileDialogOptions options;
                options.mode = FileDialogMode::Open;
                options.title = i18n::tr("control-center.devices.send-files");
                (void)FileDialog::open(
                    std::move(options), [link = m_link, id](std::optional<std::filesystem::path> path) {
                      if (path.has_value()) {
                        (void)link->sendFiles(id, {path->string()});
                      }
                    }
                );
              },
          })
      );
      row->addChild(
          ui::button({
              .glyph = "folder",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.devices.browse"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id]() { m_link->browse(id); },
          })
      );
      row->addChild(
          ui::button({
              .glyph = "screen-share",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.devices.mirror"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id]() { m_link->mirror(id); },
          })
      );
      row->addChild(
          ui::button({
              .glyph = "apps",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("control-center.devices.apps"),
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this, id = device.id]() { m_link->apps(id); },
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
    devicesCard->addChild(makeSettings(device.id, scale));
  }
  m_list->addChild(std::move(devicesCard));
  m_list->addChild(makeNearby(scale, opacity));
  m_list->layout(renderer);
}
