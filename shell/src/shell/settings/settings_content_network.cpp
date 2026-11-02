#include "shell/settings/settings_content_network.h"

#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

namespace settings {
  namespace {

    using namespace page_rows;

    // Service row: round symbol 32, name with a status line, and the row's control at the end.
    std::unique_ptr<Flex> serviceRow(std::string_view glyph, bool on, std::string_view name, std::string_view status, float scale) {
      constexpr float kIcon = 32.0F;
      auto r = row(scale);
      r->setMinHeight(Tokens::spaceP48 * scale);
      r->addChild(ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(on ? ColorRole::Accent : ColorRole::Fill2),
           .radius = kIcon * 0.5F * scale,
           .width = kIcon * scale,
           .height = kIcon * scale},
          symbol(glyph, Tokens::sizeSymbol * scale, on ? ColorRole::OnAccent : ColorRole::Label2)
      ));
      auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
      copy->addChild(makeLabel(name, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1)));
      auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
      line->addChild(ui::box({
          .fill = colorSpecFromRole(on ? ColorRole::Success : ColorRole::Label3),
          .radius = 3.0F * scale,
          .width = 6.0F * scale,
          .height = 6.0F * scale,
      }));
      line->addChild(makeLabel(status, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)));
      copy->addChild(std::move(line));
      r->addChild(std::move(copy));
      return r;
    }

  } // namespace

  void addSettingsNetwork(Flex& content, const SettingsNetworkContext& ctx) {
    const float scale = ctx.scale;
    INetworkService* net = ctx.network;
    if (net == nullptr || !net->available()) {
      addEmptyState(content, "world-off", i18n::tr("settings.network.unavailable"), scale);
      return;
    }
    const NetworkState& state = net->state();

    Flex* services = addCard(content, scale);
    {
      const bool wifiUp = state.kind == NetworkConnectivity::Wireless && state.connected;
      std::string status = !state.wirelessEnabled ? i18n::tr("settings.wifi.off")
          : wifiUp                                ? i18n::tr("settings.wifi.connected")
                                                  : i18n::tr("settings.wifi.not-connected");
      auto r = serviceRow(state.wirelessEnabled ? network_display::wifiGlyphForSignal(state.signalStrength) : "wifi-off",
                          wifiUp, i18n::tr("settings.pages.wifi"), std::move(status), scale);
      r->addChild(iconButton("chevron-right", i18n::tr("settings.pages.wifi"), scale,
                             [open = ctx.openPage] { open("wifi"); }));
      services->addChild(std::move(r));
    }
    const bool wiredUp = state.kind == NetworkConnectivity::Wired && state.connected;
    if (wiredUp || net->canActivateWiredConnection()) {
      auto r = serviceRow(wiredUp ? "ethernet" : "ethernet-off", wiredUp, i18n::tr("settings.network.ethernet"),
                          i18n::tr(wiredUp ? "settings.wifi.connected" : "settings.wifi.not-connected"), scale);
      if (!wiredUp) {
        r->addChild(plainButton(i18n::tr("settings.bluetooth.connect"), ButtonVariant::Ghost, scale,
                                [net] { net->activateWiredConnection(); }));
      }
      services->addChild(std::move(r));
    }
    if (net->canActivateCellularConnection()) {
      auto r = serviceRow("antenna-bars-5", state.cellularActive, i18n::tr("settings.network.cellular"),
                          i18n::tr(state.cellularActive ? "settings.wifi.connected" : "settings.wifi.not-connected"), scale);
      r->addChild(ui::toggle({
          .checked = state.cellularActive,
          .scale = scale,
          .onChange = [net](bool on) {
            if (on) {
              net->activateCellularConnection();
            } else {
              net->deactivateCellularConnection();
            }
          },
      }));
      services->addChild(std::move(r));
    }

    const auto& vpns = net->vpnConnections();
    if (!vpns.empty()) {
      addHeader(content, i18n::tr("settings.network.vpn"), nullptr, scale);
      Flex* card = addCard(content, scale);
      for (const auto& vpn : vpns) {
        auto r = row(scale);
        r->addChild(symbol(network_display::vpnGlyph(), Tokens::sizeSymbol * scale, ColorRole::Label2));
        r->addChild(title(vpn.name, scale));
        r->addChild(ui::toggle({
            .checked = vpn.active,
            .scale = scale,
            .onChange = [net, vpn](bool on) {
              if (on) {
                net->activateVpnConnection(vpn);
              } else {
                net->deactivateVpnConnection(vpn);
              }
            },
        }));
        card->addChild(std::move(r));
      }
    }

    if (state.connected && (!state.ipv4.empty() || !state.interfaceName.empty())) {
      addHeader(content, i18n::tr("settings.network.connection"), nullptr, scale);
      Flex* card = addCard(content, scale);
      if (!state.ipv4.empty()) {
        addValueRow(*card, i18n::tr("settings.wifi.ip-address"), state.ipv4, scale);
      }
      if (!state.interfaceName.empty()) {
        addValueRow(*card, i18n::tr("settings.wifi.interface"), state.interfaceName, scale);
      }
    }
  }

} // namespace settings
