#include "shell/settings/settings_content_wifi.h"

#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <map>
#include <vector>

namespace settings {
  namespace {

    using namespace page_rows;

    // One entry per SSID: the backend lists every BSSID; keep the strongest, and the active one when present.
    std::vector<AccessPointInfo> uniqueNetworks(const std::vector<AccessPointInfo>& aps) {
      std::map<std::string, AccessPointInfo> best;
      for (const auto& ap : aps) {
        if (ap.ssid.empty()) {
          continue;
        }
        auto [it, inserted] = best.try_emplace(ap.ssid, ap);
        if (!inserted && (ap.active || (!it->second.active && ap.strength > it->second.strength))) {
          it->second = ap;
        }
      }
      std::vector<AccessPointInfo> out;
      out.reserve(best.size());
      for (auto& [ssid, ap] : best) {
        out.push_back(std::move(ap));
      }
      std::ranges::sort(out, std::greater<>{}, &AccessPointInfo::strength);
      return out;
    }

    void addNetworkTrail(Flex& row, const AccessPointInfo& ap, float scale) {
      if (ap.secured) {
        row.addChild(symbol("lock", Tokens::sizeSymbolInline * scale, ColorRole::Label3));
      }
      row.addChild(symbol(network_display::wifiGlyphForSignal(ap.strength), Tokens::sizeSymbol * scale, ColorRole::Label2));
    }

    void addWifiHero(Flex& content, const SettingsWifiContext& ctx) {
      const float scale = ctx.scale;
      const NetworkState& state = ctx.network->state();
      std::string status;
      if (!state.wirelessEnabled) {
        status = i18n::tr("settings.wifi.off");
      } else if (state.connected && !state.ssid.empty()) {
        status = std::format("{} · {}", state.ssid, i18n::tr("settings.wifi.connected"));
      } else if (state.resolving) {
        status = i18n::tr("settings.wifi.connecting");
      } else {
        status = i18n::tr("settings.wifi.not-connected");
      }
      std::unique_ptr<Node> details;
      if (state.connected && !state.ssid.empty()) {
        details = plainButton(i18n::tr("settings.wifi.details"), ButtonVariant::Ghost, scale,
                              [open = ctx.openDetail, ssid = state.ssid] { open("wifi:" + ssid); });
      }
      addHero(
          content,
          Hero{
              .glyph = state.wirelessEnabled ? "wifi" : "wifi-off",
              .on = state.wirelessEnabled,
              .title = i18n::tr("settings.pages.wifi"),
              .status = std::move(status),
              .action = std::move(details),
              .onToggle = [network = ctx.network](bool on) { network->setWirelessEnabled(on); },
          },
          scale
      );
    }

    void addJoinRow(Flex& card, const AccessPointInfo& ap, const SettingsWifiContext& ctx) {
      const float scale = ctx.scale;
      auto r = row(scale);
      auto join = [network = ctx.network, ap, close = ctx.setJoinSsid](const std::string& psk) {
        if (psk.empty()) {
          return;
        }
        network->activateAccessPoint(ap, psk);
        close({});
      };
      r->addChild(ui::input({
          .value = ctx.password(),
          .placeholder = i18n::tr("settings.wifi.password"),
          .passwordMode = true,
          .flexGrow = 1.0F,
          .onChange = ctx.setPassword,
          .onSubmit = join,
      }));
      r->addChild(plainButton(i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, scale,
                                [close = ctx.setJoinSsid] { close({}); }));
      r->addChild(plainButton(i18n::tr("settings.wifi.join"), ButtonVariant::Default, scale,
                                [join, password = ctx.password] { join(password()); }));
      card.addChild(std::move(r));
    }

    void addLists(Flex& content, const SettingsWifiContext& ctx) {
      const float scale = ctx.scale;
      const NetworkState& state = ctx.network->state();
      std::vector<AccessPointInfo> known;
      std::vector<AccessPointInfo> other;
      for (auto& ap : uniqueNetworks(ctx.network->accessPoints())) {
        if (ap.active) {
          continue;
        }
        (ctx.network->hasSavedConnection(ap.ssid) ? known : other).push_back(std::move(ap));
      }

      if (!known.empty()) {
        addHeader(content, i18n::tr("settings.wifi.known"), nullptr, scale);
        Flex* card = addCard(content, scale);
        for (const auto& ap : known) {
          auto r = row(scale);
          r->addChild(ui::label({.text = ap.ssid, .fontSize = Style::fontSizeBody * scale,
                                   .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .flexGrow = 1.0F}));
          addNetworkTrail(*r, ap, scale);
          r->addChild(plainButton(i18n::tr("settings.wifi.join"), ButtonVariant::Ghost, scale,
                                    [network = ctx.network, ap] { network->activateAccessPoint(ap); }));
          r->addChild(iconButton("chevron-right", i18n::tr("settings.wifi.details"), scale,
                                   [open = ctx.openDetail, ssid = ap.ssid] { open("wifi:" + ssid); }));
          card->addChild(std::move(r));
        }
      }

      std::unique_ptr<Node> spinner;
      if (state.scanning) {
        spinner = ui::spinner({.spinnerSize = Tokens::sizeSymbolInline * scale, .spinning = true});
      } else {
        spinner = iconButton("refresh", i18n::tr("settings.wifi.scan"), scale,
                             [network = ctx.network] { network->requestScan(); });
      }
      addHeader(content, i18n::tr("settings.wifi.other"), std::move(spinner), scale);
      Flex* card = addCard(content, scale);
      if (other.empty()) {
        auto r = row(scale);
        r->addChild(makeLabel(i18n::tr(state.scanning ? "settings.wifi.searching" : "settings.wifi.none"),
                                Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label2)));
        card->addChild(std::move(r));
        return;
      }
      for (const auto& ap : other) {
        auto r = row(scale);
        r->addChild(ui::label({.text = ap.ssid, .fontSize = Style::fontSizeBody * scale,
                                 .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .flexGrow = 1.0F}));
        addNetworkTrail(*r, ap, scale);
        const bool asksPassword = ap.requiresCredentials() && !ap.isEnterprise();
        r->addChild(plainButton(
            i18n::tr("settings.wifi.join"), ButtonVariant::Ghost, scale,
            [network = ctx.network, ap, asksPassword, open = ctx.setJoinSsid] {
              if (asksPassword) {
                open(ap.ssid);
              } else {
                network->activateAccessPoint(ap);
              }
            }
        ));
        card->addChild(std::move(r));
        if (ctx.joinSsid == ap.ssid) {
          addJoinRow(*card, ap, ctx);
        }
      }
    }

    void addDetail(Flex& content, const SettingsWifiContext& ctx, const std::string& ssid) {
      const float scale = ctx.scale;
      const NetworkState& state = ctx.network->state();
      const bool current = state.connected && state.ssid == ssid;
      Flex* card = addCard(content, scale);
      addValueRow(*card, i18n::tr("settings.wifi.status"),
                  i18n::tr(current ? "settings.wifi.connected" : "settings.wifi.not-connected"), scale);
      if (current) {
        if (!state.ipv4.empty()) {
          addValueRow(*card, i18n::tr("settings.wifi.ip-address"), state.ipv4, scale);
        }
        if (state.frequencyMhz != 0) {
          addValueRow(*card, i18n::tr("settings.wifi.band"), network_display::wifiFrequencyBandLabel(state.frequencyMhz),
                      scale);
        }
        addValueRow(*card, i18n::tr("settings.wifi.signal"), std::format("{} %", state.signalStrength), scale);
        if (!state.interfaceName.empty()) {
          addValueRow(*card, i18n::tr("settings.wifi.interface"), state.interfaceName, scale);
        }
      }
      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingH = Style::spaceLg * scale});
      if (current) {
        actions->addChild(plainButton(i18n::tr("settings.wifi.disconnect"), ButtonVariant::Default, scale,
                                      [network = ctx.network] { network->disconnect(); }));
      }
      if (ctx.network->hasSavedConnection(ssid)) {
        actions->addChild(plainButton(i18n::tr("settings.wifi.forget"), ButtonVariant::Destructive, scale,
                                      [network = ctx.network, ssid, back = ctx.back] {
                                        network->forgetSsid(ssid);
                                        back();
                                      }));
      }
      content.addChild(std::move(actions));
    }

  } // namespace

  void addSettingsWifi(Flex& content, const SettingsWifiContext& ctx) {
    const float scale = ctx.scale;
    if (ctx.network == nullptr || !ctx.network->available()) {
      addEmptyState(content, "wifi-off", i18n::tr("settings.wifi.unavailable"), scale);
      return;
    }
    if (ctx.detail.starts_with("wifi:")) {
      addDetail(content, ctx, ctx.detail.substr(5));
      return;
    }
    addWifiHero(content, ctx);
    if (ctx.network->state().wirelessEnabled) {
      addLists(content, ctx);
    }
  }

} // namespace settings
