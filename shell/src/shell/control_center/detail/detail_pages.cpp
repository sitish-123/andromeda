#include "shell/control_center/detail/detail_pages.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/shortcut_services.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "compositors/compositor_platform.h"
#include "ipc/ipc_service.h"
#include "util/parse_number.h"
#include "util/string_utils.h"
#include "notification/notification_display_name.h"
#include "system/brightness_service.h"
#include "system/easyeffects_service.h"
#include "system/format_units.h"
#include "system/screen_time_service.h"
#include "system/system_monitor_service.h"
#include "ui/controls/countdown_ring.h"

#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/bluetooth/bluetooth_uuid_names.h"
#include "dbus/link/link_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/network/nm_hotspot.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "shell/control_center/tabs/bluetooth_tab.h"
#include "shell/control_center/tabs/power_tab.h"
#include "shell/phone/phone_state.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <vector>

namespace control_center {

  namespace {

    constexpr std::size_t kCollapsedRows = 5; // rulebook 9.16: lists show 5 rows, then "Show all"
    constexpr auto kPendingTimeout = std::chrono::seconds(6);

    std::string tr(const char* key) { return i18n::tr(key); }

    void openSettings(std::string page) { PanelManager::instance().openSettingsWindow(std::move(page)); }

    std::string title(const AudioNode& node) { return !node.description.empty() ? node.description : node.name; }

    std::string percent(float value) { return std::format("{}%", static_cast<int>(std::lround(value * 100.0F))); }

    std::unique_ptr<Flex> headerToggle(Toggle*& out, bool checked, float scale, std::function<void(bool)> onChange) {
      return ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::toggle({
              .out = &out,
              .checkedImmediate = checked,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = std::move(onChange),
          })
      );
    }

  } // namespace

  // ---- Wi-Fi ----

  bool WifiDetailPage::available() const { return m_network != nullptr && m_network->available(); }

  std::unique_ptr<Flex> WifiDetailPage::createHeaderActions() {
    return headerToggle(m_toggle, available() && m_network->state().wirelessEnabled, contentScale(), [this](bool on) {
      if (!available()) {
        return;
      }
      m_pendingEnabled = on;
      m_pendingSince = std::chrono::steady_clock::now();
      m_network->setWirelessEnabled(on);
    });
  }

  bool WifiDetailPage::deferRebuild() const {
    return m_password != nullptr
        && (!m_password->value().empty() || PanelManager::instance().inputDispatcher().focusedArea() == m_password->inputArea());
  }

  void WifiDetailPage::releaseControls() {
    m_toggle = nullptr;
    m_password = nullptr;
  }

  void WifiDetailPage::setActive(bool active) {
    if (active && !m_active && available()) {
      m_network->requestScan();
    }
    m_active = active;
    if (!active) {
      m_passwordSsid.clear();
      m_error.clear();
    }
  }

  void WifiDetailPage::sync() {
    if (m_toggle == nullptr) {
      return;
    }
    m_toggle->setEnabled(available());
    const bool enabled = available() && m_network->state().wirelessEnabled;
    if (m_pendingEnabled.has_value()) {
      if (*m_pendingEnabled == enabled || std::chrono::steady_clock::now() - m_pendingSince > kPendingTimeout) {
        m_pendingEnabled.reset();
      } else {
        return;
      }
    }
    m_toggle->setChecked(enabled);
  }

  std::string WifiDetailPage::structureKey() const {
    if (!available()) {
      return "unavailable";
    }
    const NetworkState& state = m_network->state();
    std::string key = std::format("{}|{}|{}|{}|{}|{}|{}|{}", state.wirelessEnabled, state.resolving, state.ssid,
                                  m_showAll, m_passwordSsid, m_error, state.ipv4, state.frequencyMhz);
    key += std::format("|{}:{}:{}:{}", static_cast<int>(state.kind), state.connected,
                       m_network->canActivateWiredConnection(),
                       m_hotspot != nullptr ? std::format("{}:{}", static_cast<int>(m_hotspot->state()), m_hotspot->lastError())
                                            : std::string());
    if (state.wirelessEnabled) {
      for (const AccessPointInfo& ap : m_network->accessPoints()) {
        key += std::format("|{}:{}:{}:{}:{}", ap.ssid, network_display::wifiSignalBand(ap.strength), ap.secured,
                           ap.active, m_network->hasSavedConnection(ap.ssid));
      }
    }
    return key;
  }

  void WifiDetailPage::build() {
    m_password = nullptr;
    if (!available()) {
      addEmpty("wifi-off", tr("control-center.detail.wifi.unavailable"), tr("control-center.detail.wifi.unavailable-body"));
      return;
    }
    buildWireless();
    addLinks();
  }

  void WifiDetailPage::addLinks() {
    const NetworkState& state = m_network->state();
    const bool wired = state.kind == NetworkConnectivity::Wired && state.connected;
    if (!wired && !m_network->canActivateWiredConnection() && m_hotspot == nullptr) {
      return;
    }
    if (m_hotspot != nullptr && !m_hotspot->lastError().empty()) {
      addError(m_hotspot->lastError(), [this] {
        (void)m_hotspot->setEnabled(m_hotspot->requestedOn());
        requestRebuild();
      });
    }
    Flex* card = addGroup();
    if (m_hotspot != nullptr) {
      DetailRow row;
      row.glyph = "access-point";
      row.title = tr("control-center.detail.wifi.hotspot");
      row.toggle = m_hotspot->active();
      row.toggleEnabled = !m_hotspot->pending();
      row.onToggle = [this](bool on) {
        (void)m_hotspot->setEnabled(on);
        requestRebuild();
      };
      addRow(*card, row);
    }
    if (wired || m_network->canActivateWiredConnection()) {
      DetailRow row;
      row.glyph = wired ? "ethernet" : "ethernet-off";
      row.title = tr("control-center.network.ethernet");
      row.trailing = tr(wired ? "control-center.network.connected" : "control-center.network.not-connected");
      if (wired && !state.ipv4.empty()) {
        row.secondary = state.ipv4;
      }
      row.selected = wired;
      row.onClick = [this] {
        const NetworkState& now = m_network->state();
        if (now.kind == NetworkConnectivity::Wired && now.connected) {
          if (!m_network->deactivateWiredConnection()) {
            m_error = i18n::tr("control-center.detail.wifi.disconnect-failed", "name",
                               tr("control-center.network.ethernet"));
          }
        } else if (!m_network->activateWiredConnection()) {
          m_error = i18n::tr("control-center.detail.wifi.connect-failed", "ssid", tr("control-center.network.ethernet"));
        }
        requestRebuild();
      };
      addRow(*card, row);
    }
  }

  void WifiDetailPage::buildWireless() {
    const NetworkState& state = m_network->state();
    if (!state.wirelessEnabled) {
      addEmpty("wifi-off", tr("control-center.detail.wifi.off"), tr("control-center.detail.wifi.off-body"));
      return;
    }
    if (!m_error.empty()) {
      addError(m_error, [this] {
        m_error.clear();
        requestRebuild();
      });
    }

    std::vector<AccessPointInfo> aps = m_network->accessPoints();
    // Active first, then by signal band and name: the raw percent would reshuffle rows on every scan.
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_display::wifiSignalBand(a.strength);
      const int bandB = network_display::wifiSignalBand(b.strength);
      return bandA != bandB ? bandA > bandB : a.ssid < b.ssid;
    });

    if (!aps.empty() && aps.front().active) {
      const AccessPointInfo& active = aps.front();
      Flex* card = addGroup(tr("control-center.detail.wifi.connected"));
      DetailRow row;
      row.glyph = network_display::wifiGlyphForSignal(active.strength);
      row.title = active.ssid;
      // "Connected · 5 GHz · Secured": band from the associated BSS frequency.
      std::string status = tr("control-center.detail.wifi.status-connected");
      if (state.frequencyMhz >= 5925) {
        status += " · 6 GHz";
      } else if (state.frequencyMhz >= 4900) {
        status += " · 5 GHz";
      } else if (state.frequencyMhz > 0) {
        status += " · 2.4 GHz";
      }
      if (active.secured) {
        status += " · " + tr("control-center.detail.wifi.secured");
      }
      row.secondary = state.resolving ? tr("control-center.quick.connecting") : status;
      row.selected = true;
      row.action = tr("control-center.detail.wifi.disconnect");
      row.onAction = [this] {
        m_network->disconnect();
        requestRebuild();
      };
      addRow(*card, row);
      if (!state.ipv4.empty()) {
        DetailRow ip;
        ip.title = tr("control-center.detail.wifi.ip-address");
        ip.trailing = state.ipv4;
        addRow(*card, ip);
      }
      DetailRow details;
      details.title = tr("control-center.detail.wifi.network-details");
      details.trailingGlyphs = {Style::rtl() ? "chevron-left" : "chevron-right"};
      details.onClick = [] { openSettings("wifi"); };
      addRow(*card, details);
      if (m_network->hasSavedConnection(active.ssid)) {
        DetailRow forget;
        forget.title = tr("control-center.detail.wifi.forget");
        forget.onClick = [this, ssid = active.ssid] {
          m_network->forgetSsid(ssid);
          requestRebuild();
        };
        addRow(*card, forget);
      }
    }

    std::vector<const AccessPointInfo*> known;
    std::vector<const AccessPointInfo*> others;
    for (const AccessPointInfo& ap : aps) {
      if (!ap.active) {
        (m_network->hasSavedConnection(ap.ssid) ? known : others).push_back(&ap);
      }
    }
    if (known.empty() && others.empty()) {
      if (aps.empty()) {
        addEmpty("wifi", tr("control-center.detail.wifi.none"), "");
      }
      return;
    }
    const auto addAccessPoint = [this](Flex* card, const AccessPointInfo ap) {
      const bool saved = m_network->hasSavedConnection(ap.ssid);
      DetailRow row;
      row.glyph = network_display::wifiGlyphForSignal(ap.strength);
      row.title = ap.ssid;
      if (ap.isEnterprise()) {
        row.secondary = tr("control-center.detail.wifi.needs-settings");
      }
      if (ap.secured) {
        row.trailingGlyphs.push_back("lock");
      }
      row.onClick = [this, ap, saved] {
        m_error.clear();
        if (ap.isEnterprise()) {
          openSettings("wifi");
          return;
        }
        if (ap.requiresCredentials() && !saved) {
          m_passwordSsid = m_passwordSsid == ap.ssid ? std::string{} : ap.ssid;
        } else if (!m_network->activateAccessPoint(ap)) {
          m_error = i18n::tr("control-center.detail.wifi.connect-failed", "ssid", ap.ssid);
        }
        requestRebuild();
      };
      addRow(*card, row);
      if (m_passwordSsid != ap.ssid) {
        return;
      }
      const float s = contentScale();
      auto form = std::make_unique<Flex>();
      form->setDirection(FlexDirection::Horizontal);
      form->setAlign(FlexAlign::Center);
      form->setGap(Tokens::spaceP8 * s);
      form->setPadding(Tokens::spaceP4 * s, Tokens::componentListRowInset * s, Tokens::spaceP8 * s,
                       Tokens::componentListRowInset * s);
      const auto join = [this, ap](const std::string& psk) {
        if (psk.empty()) {
          return;
        }
        m_passwordSsid.clear();
        if (!m_network->activateAccessPoint(ap, psk)) {
          m_error = i18n::tr("control-center.detail.wifi.connect-failed", "ssid", ap.ssid);
        }
        requestRebuild();
      };
      form->addChild(ui::input({
          .out = &m_password,
          .placeholder = tr("control-center.network.password"),
          .controlHeight = Tokens::componentFieldHeight * s,
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Fill2,
          .flexGrow = 1.0F,
          .onSubmit = join,
      }));
      // Field and button share the row's height and the r 8 of the rows around them.
      m_password->setFrameRadius(Tokens::componentFieldRadius);
      auto joinButton = std::make_unique<Button>();
      joinButton->setVariant(ButtonVariant::Primary);
      joinButton->setText(tr("control-center.network.join"));
      joinButton->setControlHeight(Tokens::componentFieldHeight * s);
      joinButton->setRadius(Tokens::componentButtonRadius * s);
      joinButton->setPadding(0.0F, Tokens::componentButtonInsetM * s, 0.0F, Tokens::componentButtonInsetM * s);
      joinButton->setOnClick([this, join] {
        if (m_password != nullptr) {
          join(m_password->value());
        }
      });
      form->addChild(std::move(joinButton));
      card->addChild(std::move(form));
    };
    if (!known.empty()) {
      Flex* card = addGroup(tr("control-center.detail.wifi.known-networks"));
      for (const AccessPointInfo* ap : known) {
        addAccessPoint(card, *ap);
      }
    }
    if (others.empty()) {
      return;
    }
    Flex* card = addGroup(tr("control-center.detail.wifi.other-networks"));
    const std::size_t shown = m_showAll ? others.size() : std::min(others.size(), kCollapsedRows);
    for (std::size_t i = 0; i < shown; ++i) {
      addAccessPoint(card, *others[i]);
    }
    if (others.size() > kCollapsedRows) {
      addShowAll(*card, m_showAll, [this] {
        m_showAll = !m_showAll;
        requestRebuild();
      });
    }
  }

  std::vector<DetailPage::FooterButton> WifiDetailPage::footerButtons() {
    return {
        {tr("control-center.detail.wifi.other-network"), [] { openSettings("wifi"); }},
        {tr("control-center.detail.wifi.settings"), [] { openSettings("wifi"); }},
    };
  }

  // ---- Bluetooth ----

  std::unique_ptr<Flex> BluetoothDetailPage::createHeaderActions() {
    const bool on = m_bluetooth != nullptr && m_bluetooth->state().powered;
    return headerToggle(m_toggle, on, contentScale(), [this](bool enabled) {
      if (m_bluetooth != nullptr) {
        m_bluetooth->setPowered(enabled);
      }
    });
  }

  void BluetoothDetailPage::stopScan() {
    if (m_startedScan && m_bluetooth != nullptr && m_bluetooth->state().discovering) {
      m_bluetooth->stopDiscovery();
    }
    m_startedScan = false;
  }

  void BluetoothDetailPage::startScan() {
    constexpr auto kRetry = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    if (m_bluetooth == nullptr || m_bluetooth->state().discovering || now - m_scanAttempt < kRetry) {
      return;
    }
    m_scanAttempt = now;
    m_bluetooth->startDiscovery();
    m_startedScan = true;
  }

  void BluetoothDetailPage::setActive(bool active) {
    m_active = active;
    if (!active) {
      stopScan();
      m_showNearby = false;
      m_error.clear();
    }
  }

  void BluetoothDetailPage::onClose() {
    m_active = false;
    stopScan();
    m_showNearby = false;
    DetailPage::onClose();
  }

  void BluetoothDetailPage::sync() {
    if (m_bluetooth == nullptr) {
      return;
    }
    const BluetoothState& state = m_bluetooth->state();
    if (m_toggle != nullptr) {
      // No adapter: no switch at all, the empty state says why (a disabled switch reads as broken).
      m_toggle->setVisible(state.adapterPresent);
      m_toggle->setChecked(state.powered);
    }
    setFooterVisible(state.adapterPresent);
    // The page scans while it is open; BlueZ may end a scan on its own, so the state decides.
    if (m_active && state.adapterPresent && state.powered) {
      startScan();
    } else if (!state.powered) {
      m_startedScan = false;
    }
  }

  std::string BluetoothDetailPage::structureKey() const {
    if (m_bluetooth == nullptr || !m_bluetooth->state().adapterPresent) {
      return "unavailable";
    }
    const BluetoothState& state = m_bluetooth->state();
    std::string key = std::format("{}|{}|{}|{}|{}|{}", state.powered, state.discovering, state.discoverable, m_showNearby,
                                  m_showAllPaired, m_error);
    if (m_agent != nullptr && m_agent->hasPendingRequest()) {
      const BluetoothPairingRequest req = m_agent->pendingRequest();
      key += std::format("|pair:{}:{}:{}:{}", static_cast<int>(req.kind), req.devicePath, req.passkey, req.pin);
    }
    for (const BluetoothDeviceInfo& d : m_bluetooth->devices()) {
      key += std::format("|{}:{}:{}:{}:{}:{}:{}", d.path, d.alias, d.paired, d.connected, d.connecting, d.hasBattery,
                         d.batteryPercent);
    }
    return key;
  }

  bool BluetoothDetailPage::deferRebuild() const {
    return m_code != nullptr
        && (!m_code->value().empty() || PanelManager::instance().inputDispatcher().focusedArea() == m_code->inputArea());
  }

  // The device's pairing question (.agent/DESIGN.md "Control Center pages", gaps): code to compare, PIN or passkey to
  // type, or a yes/no; it shows while the agent holds a request and this page is open (Pair starts here).
  void BluetoothDetailPage::addPairingPrompt() {
    const BluetoothPairingRequest req = m_agent->pendingRequest();
    std::string alias = req.devicePath;
    for (const BluetoothDeviceInfo& d : m_bluetooth->devices()) {
      if (d.path == req.devicePath && !d.alias.empty()) {
        alias = d.alias;
      }
    }
    const float s = contentScale();
    Flex* card = addGroup();
    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP12 * s});
    body->addChild(ui::label({.text = i18n::tr("control-center.bluetooth.pair-title", "device", alias),
                              .fontSize = Tokens::typeHeadline.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::End}));
    std::string detail;
    switch (req.kind) {
    case BluetoothPairingKind::Confirm: detail = tr("control-center.bluetooth.pairing-detail.confirm"); break;
    case BluetoothPairingKind::Authorize: detail = tr("control-center.bluetooth.pairing-detail.authorize"); break;
    case BluetoothPairingKind::AuthorizeService:
      detail = i18n::tr("control-center.bluetooth.pairing-detail.authorize-service", "uuid", bluetoothServiceLabel(req.uuid));
      break;
    case BluetoothPairingKind::DisplayPinCode: detail = tr("control-center.bluetooth.pairing-detail.display-pin"); break;
    case BluetoothPairingKind::DisplayPasskey: detail = tr("control-center.bluetooth.pairing-detail.display-passkey"); break;
    case BluetoothPairingKind::PinCode: detail = tr("control-center.bluetooth.pairing-detail.pin-code"); break;
    case BluetoothPairingKind::Passkey: detail = tr("control-center.bluetooth.pairing-detail.passkey"); break;
    case BluetoothPairingKind::None: break;
    }
    body->addChild(ui::label({.text = detail, .fontSize = Tokens::typeFootnote.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                              .maxLines = 3}));
    const bool needsInput = req.kind == BluetoothPairingKind::PinCode || req.kind == BluetoothPairingKind::Passkey;
    const bool showsCode = req.kind == BluetoothPairingKind::Confirm || req.kind == BluetoothPairingKind::DisplayPasskey
        || req.kind == BluetoothPairingKind::DisplayPinCode;
    if (showsCode) {
      const std::string code = req.kind == BluetoothPairingKind::DisplayPinCode ? req.pin : std::format("{:06}", req.passkey);
      body->addChild(ui::label({.text = code, .fontSize = Tokens::typeTitle2.size * s, .fontWeight = FontWeight::SemiBold,
                                .color = colorSpecFromRole(ColorRole::Label1)}));
    }
    const auto submit = [this, kind = req.kind](const std::string& value) {
      if (kind == BluetoothPairingKind::PinCode) {
        m_agent->submitPin(value);
      } else if (const auto passkey = parseNumber<std::uint32_t>(StringUtils::trim(value)); passkey.has_value() && *passkey <= 999999) {
        m_agent->submitPasskey(*passkey);
      } else {
        m_agent->cancelPending();
      }
      m_code = nullptr;
      requestRebuild();
    };
    if (needsInput) {
      body->addChild(ui::input({
          .out = &m_code,
          .placeholder = tr("control-center.detail.bluetooth.code"),
          .controlHeight = Tokens::componentFieldHeight * s,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::Fill2,
          .onSubmit = submit,
      }));
      m_code->setCapsule(true);
    }
    auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Tokens::spaceP8 * s});
    const auto button = [s](const std::string& text, bool primary, std::function<void()> onClick) {
      auto b = std::make_unique<Button>();
      b->setVariant(primary ? ButtonVariant::Primary : ButtonVariant::Default);
      b->setText(text);
      b->setControlHeight(Tokens::componentButtonHeightM * s);
      b->setRadius(Tokens::componentButtonRadius * s);
      b->setOnClick(std::move(onClick));
      return b;
    };
    const bool yesNo = req.kind == BluetoothPairingKind::Confirm || req.kind == BluetoothPairingKind::Authorize
        || req.kind == BluetoothPairingKind::AuthorizeService;
    buttons->addChild(button(tr("control-center.detail.bluetooth.cancel"), false, [this, yesNo] {
      if (yesNo) {
        m_agent->rejectConfirm();
      } else {
        m_agent->cancelPending();
      }
      requestRebuild();
    }));
    if (yesNo || req.kind == BluetoothPairingKind::DisplayPinCode) {
      buttons->addChild(button(tr("control-center.detail.bluetooth.pair"), true, [this] {
        m_agent->acceptConfirm();
        requestRebuild();
      }));
    } else if (needsInput) {
      buttons->addChild(button(tr("control-center.detail.bluetooth.pair"), true, [this, submit] {
        if (m_code != nullptr) {
          submit(m_code->value());
        }
      }));
    }
    body->addChild(std::move(buttons));
    card->addChild(std::move(body));
  }

  void BluetoothDetailPage::build() {
    if (m_bluetooth == nullptr || !m_bluetooth->state().adapterPresent) {
      addEmpty("bluetooth-off", tr("control-center.detail.bluetooth.unavailable"),
               tr("control-center.detail.bluetooth.unavailable-body"));
      return;
    }
    if (m_agent != nullptr && m_agent->hasPendingRequest()) {
      addPairingPrompt();
    }
    if (!m_bluetooth->state().powered) {
      addEmpty("bluetooth-off", tr("control-center.detail.bluetooth.off"), tr("control-center.detail.bluetooth.off-body"));
      return;
    }
    // Status line "Connected · 82 %" and one trailing action (Disconnect, Connect, Pair) per device.
    const auto deviceRow = [this](const BluetoothDeviceInfo& d, std::function<void()> click) {
      DetailRow row;
      row.glyph = bluetoothDeviceGlyph(d.kind);
      row.title = d.alias;
      if (d.connecting) {
        row.secondary = tr("control-center.quick.connecting");
      } else if (d.connected) {
        row.secondary = tr("control-center.detail.bluetooth.status-connected");
        if (d.hasBattery) {
          row.secondary += std::format(" · {} %", d.batteryPercent);
        }
      } else if (d.paired) {
        row.secondary = tr("control-center.detail.bluetooth.status-not-connected");
      }
      row.selected = d.connected;
      row.action = tr(d.connected ? "control-center.detail.bluetooth.disconnect"
                      : d.paired  ? "control-center.detail.bluetooth.connect"
                                  : "control-center.detail.bluetooth.pair");
      row.onAction = click;
      row.onClick = std::move(click);
      return row;
    };
    std::vector<BluetoothDeviceInfo> connected;
    std::vector<BluetoothDeviceInfo> paired;
    std::vector<BluetoothDeviceInfo> nearby;
    for (const BluetoothDeviceInfo& d : m_bluetooth->devices()) {
      (d.connected ? connected : d.paired ? paired : nearby).push_back(d);
    }
    if (!connected.empty()) {
      Flex* card = addGroup(tr("control-center.detail.bluetooth.connected-devices"));
      for (const BluetoothDeviceInfo& d : connected) {
        addRow(*card, deviceRow(d, [this, path = d.path] { m_bluetooth->disconnectDevice(path); }));
      }
    }
    if (!paired.empty()) {
      Flex* card = addGroup(tr("control-center.detail.bluetooth.paired"));
      const std::size_t shown = m_showAllPaired ? paired.size() : std::min(paired.size(), kCollapsedRows);
      for (std::size_t i = 0; i < shown; ++i) {
        addRow(*card, deviceRow(paired[i], [this, path = paired[i].path] { m_bluetooth->connect(path); }));
      }
      if (paired.size() > kCollapsedRows) {
        addShowAll(*card, m_showAllPaired, [this] {
          m_showAllPaired = !m_showAllPaired;
          requestRebuild();
        });
      }
    }
    if (m_showNearby) {
      Flex* card = addGroup(tr("control-center.detail.bluetooth.nearby"));
      for (const BluetoothDeviceInfo& d : nearby) {
        addRow(*card, deviceRow(d, [this, path = d.path] { m_bluetooth->pair(path); }));
      }
      if (nearby.empty()) {
        DetailRow row;
        row.title = tr("control-center.detail.bluetooth.scanning");
        addRow(*card, row);
      }
    }
    if (connected.empty() && paired.empty() && !m_showNearby) {
      addEmpty("bluetooth", tr("control-center.bluetooth.no-devices"), "");
    }
    DetailRow visible;
    visible.title = tr("control-center.detail.bluetooth.visible");
    visible.toggle = m_bluetooth->state().discoverable;
    visible.onToggle = [this](bool on) {
      m_error.clear();
      m_bluetooth->setDiscoverable(on, [this, alive = std::weak_ptr<int>(m_lifetime)](std::string error) {
        if (alive.expired()) {
          return;
        }
        m_error = std::move(error);
        requestRebuild();
      });
    };
    if (!m_error.empty()) {
      addError(m_error, [this] {
        m_error.clear();
        requestRebuild();
      });
    }
    addRow(*addGroup(), visible);
  }

  std::vector<DetailPage::FooterButton> BluetoothDetailPage::footerButtons() {
    return {
        {tr("control-center.detail.bluetooth.pair-new"),
         [this] {
           m_showNearby = true;
           startScan();
           requestRebuild();
         }},
        {tr("control-center.detail.bluetooth.settings"), [] { openSettings("bluetooth"); }},
    };
  }

  // ---- Sound ----

  std::string SoundDetailPage::structureKey() const {
    if (m_audio == nullptr) {
      return "unavailable";
    }
    const AudioState& state = m_audio->state();
    std::string key = std::format("{}|{}|{}|{}", m_showAllOutputs, m_showAllApps, state.defaultSinkId, state.defaultSourceId);
    for (const AudioNode& n : state.sinks) {
      key += std::format("|o{}:{}:{}:{}", n.id, title(n), n.description, n.available);
    }
    for (const AudioNode& n : state.sources) {
      key += std::format("|i{}:{}", n.id, title(n));
    }
    for (const AudioNode& n : state.programOutputs) {
      key += std::format("|p{}:{}:{}:{}:{}:{}", n.id, n.applicationName, n.iconName, n.applicationId, n.applicationBinary, title(n));
    }
    if (m_effects != nullptr) {
      key += std::format("|e{}", static_cast<int>(m_openEffects));
      for (const AudioEffectsProfileKind kind : {AudioEffectsProfileKind::Output, AudioEffectsProfileKind::Input}) {
        key += std::format("|{}", m_effects->activeEffectsProfile(kind));
        for (const std::string& p : m_effects->effectsProfiles(kind)) {
          key += ":" + p;
        }
      }
    }
    return key;
  }

  void SoundDetailPage::setActive(bool active) {
    if (active && m_effects != nullptr) {
      m_effects->refreshAsync();
    }
    if (!active) {
      m_openEffects = EffectsList::None;
    }
  }

  // An "Effects" row with the active EasyEffects profile; a click opens the profile list inline. None without profiles.
  void SoundDetailPage::addEffectsRows(Flex& card, EffectsList list) {
    if (m_effects == nullptr) {
      return;
    }
    const AudioEffectsProfileKind kind = list == EffectsList::Input ? AudioEffectsProfileKind::Input : AudioEffectsProfileKind::Output;
    const std::vector<std::string>& profiles = m_effects->effectsProfiles(kind);
    if (profiles.empty()) {
      return;
    }
    const bool open = m_openEffects == list;
    const std::string active = m_effects->activeEffectsProfile(kind);
    DetailRow row;
    row.glyph = list == EffectsList::Input ? "microphone" : "volume";
    row.title = tr("control-center.audio.effects-profile");
    row.trailing = active;
    row.trailingGlyphs = {open ? "chevron-up" : "chevron-down"};
    row.onClick = [this, list, open] {
      m_openEffects = open ? EffectsList::None : list;
      requestRebuild();
    };
    addRow(card, row);
    if (!open) {
      return;
    }
    for (const std::string& profile : profiles) {
      DetailRow item;
      item.title = profile;
      item.selected = profile == active;
      item.onClick = [this, kind, profile] {
        if (m_effects->loadEffectsProfile(kind, profile)) {
          m_effects->refreshAsync();
        }
        m_openEffects = EffectsList::None;
        requestRebuild();
      };
      addRow(card, item);
    }
  }

  void SoundDetailPage::build() {
    if (m_audio == nullptr) {
      addEmpty("volume-off", tr("control-center.detail.sound.unavailable"), tr("control-center.detail.sound.unavailable-body"));
      return;
    }
    const AudioState& state = m_audio->state();
    const auto level = [this](std::uint32_t id, bool input) {
      return [this, id, input]() -> std::optional<float> {
        for (const AudioNode& n : input ? m_audio->state().sources : m_audio->state().sinks) {
          if (n.id == id) {
            return std::min(n.volume, 1.0F);
          }
        }
        return std::nullopt;
      };
    };

    std::vector<const AudioNode*> outputs;
    for (const AudioNode& n : state.sinks) {
      if (n.available || n.id == state.defaultSinkId) {
        outputs.push_back(&n);
      }
    }
    Flex* card = addGroup(tr("control-center.detail.sound.output"));
    if (outputs.empty()) {
      DetailRow row;
      row.title = tr("control-center.audio.no-output-devices");
      addRow(*card, row);
    }
    const std::size_t shownOutputs = m_showAllOutputs ? outputs.size() : std::min(outputs.size(), kCollapsedRows);
    for (std::size_t i = 0; i < shownOutputs; ++i) {
      const AudioNode& n = *outputs[i];
      DetailRow row;
      row.glyph = "volume";
      row.title = title(n);
      row.selected = n.id == state.defaultSinkId;
      row.onClick = [this, id = n.id] { m_audio->setDefaultSink(id); };
      addRow(*card, row);
      addSliderRow(*card, {.glyph = "volume",
                           .title = tr("control-center.detail.sound.output"),
                           .value = level(n.id, false),
                           .onChange = [this, id = n.id](float v) { m_audio->setSinkVolume(id, v); }});
    }
    if (outputs.size() > kCollapsedRows) {
      addShowAll(*card, m_showAllOutputs, [this] {
        m_showAllOutputs = !m_showAllOutputs;
        requestRebuild();
      });
    }
    addEffectsRows(*card, EffectsList::Output);

    if (!state.programOutputs.empty()) {
      Flex* apps = addGroup(tr("control-center.detail.sound.applications"));
      const std::size_t shown = m_showAllApps ? state.programOutputs.size() : std::min(state.programOutputs.size(), kCollapsedRows);
      for (std::size_t i = 0; i < shown; ++i) {
        const AudioNode& n = state.programOutputs[i];
        DetailSliderRow row;
        row.glyph = "volume";
        for (const std::string& candidate : {n.iconName, n.applicationId, n.applicationBinary}) {
          if (!candidate.empty()) {
            row.iconCandidates.push_back(candidate);
          }
        }
        if (row.iconCandidates.empty()) {
          row.iconCandidates.emplace_back("application-default-icon");
        }
        row.title = !n.applicationName.empty() ? n.applicationName : title(n);
        row.value = [this, id = n.id]() -> std::optional<float> {
          for (const AudioNode& p : m_audio->state().programOutputs) {
            if (p.id == id) {
              return std::min(p.volume, 1.0F);
            }
          }
          return std::nullopt;
        };
        row.onChange = [this, id = n.id](float v) { m_audio->setProgramOutputVolume(id, v); };
        addSliderRow(*apps, row);
      }
      if (state.programOutputs.size() > kCollapsedRows) {
        addShowAll(*apps, m_showAllApps, [this] {
          m_showAllApps = !m_showAllApps;
          requestRebuild();
        });
      }
    }

    Flex* inputs = addGroup(tr("control-center.detail.sound.input"));
    if (state.sources.empty()) {
      DetailRow row;
      row.title = tr("control-center.detail.sound.no-input");
      addRow(*inputs, row);
    }
    for (const AudioNode& n : state.sources) {
      DetailRow row;
      row.glyph = "microphone";
      row.title = title(n);
      row.selected = n.id == state.defaultSourceId;
      row.onClick = [this, id = n.id] { m_audio->setDefaultSource(id); };
      addRow(*inputs, row);
    }
    if (const AudioNode* source = m_audio->defaultSource(); source != nullptr) {
      addSliderRow(*inputs, {.glyph = "microphone",
                             .title = tr("control-center.detail.sound.microphone"),
                             .value = level(source->id, true),
                             .onChange = [this, id = source->id](float v) { m_audio->setSourceVolume(id, v); }});
    }
    addEffectsRows(*inputs, EffectsList::Input);
  }

  std::vector<DetailPage::FooterButton> SoundDetailPage::footerButtons() {
    return {{tr("control-center.detail.sound.settings"), [] { openSettings("sound"); }}};
  }

  // ---- Power ----

  namespace {
    std::string batteryName(const UPowerDeviceInfo& info) {
      return !info.model.empty() ? info.model : !info.vendor.empty() ? info.vendor : tr("control-center.power.unknown-device");
    }

    // Laptop batteries whose charge limit UPower can switch, and present peripheral batteries.
    struct PowerBatteries {
      std::vector<UPowerDeviceInfo> limits;
      std::vector<UPowerDeviceInfo> peripherals;
    };

    PowerBatteries powerBatteries(UPowerService* upower) {
      PowerBatteries out;
      if (upower == nullptr) {
        return out;
      }
      for (UPowerDeviceInfo& d : upower->batteryDevices()) {
        if (!d.isPresent) {
          continue;
        }
        if (!d.isLaptopBattery()) {
          out.peripherals.push_back(std::move(d));
        } else if (PowerTab::chargeLimitControlState(d.chargeLimit).visible) {
          out.limits.push_back(std::move(d));
        }
      }
      return out;
    }
  } // namespace

  std::string PowerDetailPage::structureKey() const {
    const bool hasProfiles = m_profiles != nullptr && !m_profiles->profiles().empty();
    const PowerBatteries batteries = powerBatteries(m_upower);
    if (!hasProfiles && batteries.limits.empty() && batteries.peripherals.empty()) {
      return "unavailable";
    }
    std::string key;
    if (hasProfiles) {
      key = m_profiles->activeProfile();
      for (const std::string& p : m_profiles->profiles()) {
        key += "|" + p;
      }
    }
    if (m_upower != nullptr) {
      if (const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery(); battery != nullptr) {
        key += std::format("|{}", battery->healthPercent().value_or(-1.0));
      }
    }
    for (const UPowerDeviceInfo& d : batteries.limits) {
      const PowerTab::ChargeLimitControlState c = PowerTab::chargeLimitControlState(d.chargeLimit);
      key += std::format("|l{}:{}:{}:{}", d.path, batteryName(d), c.checked, c.enabled);
    }
    for (const UPowerDeviceInfo& d : batteries.peripherals) {
      key += std::format("|p{}:{}:{}:{}", d.path, batteryName(d), static_cast<int>(d.type),
                         std::lround(d.state.percentage));
    }
    return key;
  }

  void PowerDetailPage::build() {
    const bool hasProfiles = m_profiles != nullptr && !m_profiles->profiles().empty();
    const PowerBatteries batteries = powerBatteries(m_upower);
    if (!hasProfiles && batteries.limits.empty() && batteries.peripherals.empty()) {
      addEmpty("battery-off", tr("control-center.detail.power.unavailable"), tr("control-center.detail.power.unavailable-body"));
      return;
    }
    if (hasProfiles) {
      Flex* card = addGroup(tr("control-center.detail.power.modes"));
      for (const std::string_view profile : powerProfileOrder()) {
        if (!std::ranges::contains(m_profiles->profiles(), profile)) {
          continue;
        }
        DetailRow row;
        row.glyph = std::string(profileGlyphName(profile));
        row.title = profileLabel(profile);
        row.selected = m_profiles->activeProfile() == profile;
        row.onClick = [this, name = std::string(profile)] {
          if (m_profiles->setActiveProfile(name)) {
            requestRebuild();
          }
        };
        addRow(*card, row);
      }
      if (m_upower != nullptr) {
        if (const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery(); battery != nullptr) {
          if (const auto health = battery->healthPercent(); health.has_value()) {
            addGroupFooter(*card,
                           i18n::tr("control-center.detail.power.health", "percent", static_cast<int>(std::lround(*health))));
          }
        }
      }
    }
    if (!batteries.limits.empty()) {
      Flex* card = addGroup();
      for (const UPowerDeviceInfo& d : batteries.limits) {
        const PowerTab::ChargeLimitControlState c = PowerTab::chargeLimitControlState(d.chargeLimit);
        DetailRow row;
        row.glyph = batteryGlyphName(d.state.percentage, d.state.state);
        row.title = tr("control-center.detail.power.charge-limit");
        if (batteries.limits.size() > 1) {
          row.secondary = batteryName(d);
        }
        row.toggle = c.checked;
        row.toggleEnabled = c.enabled;
        row.onToggle = [this, path = d.path](bool on) {
          (void)m_upower->enableChargeThreshold(path, on);
          requestRebuild();
        };
        addRow(*card, row);
      }
    }
    if (!batteries.peripherals.empty()) {
      Flex* card = addGroup(tr("control-center.power.peripherals"));
      for (const UPowerDeviceInfo& d : batteries.peripherals) {
        DetailRow row;
        row.glyph = batteryDeviceGlyphName(d.type);
        row.title = batteryName(d);
        row.trailing = std::format("{} %", std::lround(d.state.percentage));
        addRow(*card, row);
      }
    }
  }

  std::vector<DetailPage::FooterButton> PowerDetailPage::footerButtons() {
    return {{tr("control-center.detail.power.settings"), [] { openSettings("power"); }}};
  }

  // ---- Focus ----

  std::unique_ptr<Flex> FocusDetailPage::createHeaderActions() {
    const bool on = m_notifications != nullptr && m_notifications->doNotDisturb();
    return headerToggle(m_toggle, on, contentScale(), [this](bool enabled) {
      if (enabled) {
        enable(Until::Manual);
      } else {
        disable();
      }
    });
  }

  void FocusDetailPage::enable(Until until) {
    if (m_notifications == nullptr) {
      return;
    }
    m_timer.stop();
    m_until = until;
    m_notifications->setDoNotDisturb(true, until == Until::OneHour || until == Until::Tomorrow);
    m_revision = m_notifications->doNotDisturbRevision();
    if (until == Until::OneHour || until == Until::Tomorrow) {
      auto end = std::chrono::system_clock::now() + std::chrono::hours(1);
      if (until == Until::Tomorrow) {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        local.tm_mday += 1;
        local.tm_hour = 8;
        local.tm_min = 0;
        local.tm_sec = 0;
        local.tm_isdst = -1;
        end = std::chrono::system_clock::from_time_t(std::mktime(&local));
      }
      m_endsAt = end;
      m_timer.start(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::system_clock::now()),
                    [this] {
                      // Another switch (the Focus tile) changed Do Not Disturb since: this choice is void.
                      if (m_notifications != nullptr && m_notifications->doNotDisturbRevision() == m_revision) {
                        disable();
                      }
                      m_until = Until::None;
                      if (PanelManager::instance().isOpenPanel("control-center")) {
                        requestRebuild();
                      }
                    });
    }
    requestRebuild();
  }

  void FocusDetailPage::disable() {
    m_timer.stop();
    m_until = Until::None;
    if (m_notifications != nullptr) {
      m_notifications->setDoNotDisturb(false);
    }
    if (PanelManager::instance().isOpenPanel("control-center")) {
      requestRebuild();
    }
  }

  void FocusDetailPage::sync() {
    if (m_notifications == nullptr) {
      return;
    }
    const bool on = m_notifications->doNotDisturb();
    if (!on && m_until != Until::None) {
      m_timer.stop();
      m_until = Until::None;
    }
    if (m_toggle != nullptr) {
      m_toggle->setChecked(on);
    }
  }

  std::string FocusDetailPage::structureKey() const {
    const bool on = m_notifications != nullptr && m_notifications->doNotDisturb();
    return std::format("{}|{}", on, static_cast<int>(m_until));
  }

  void FocusDetailPage::build() {
    if (m_notifications == nullptr) {
      return;
    }
    const bool on = m_notifications->doNotDisturb();
    const Until current = on && m_until == Until::None ? Until::Manual : m_until;
    Flex* mode = addGroup();
    DetailRow dnd;
    dnd.glyph = "moon";
    dnd.title = tr("control-center.detail.focus.dnd");
    dnd.selected = on;
    dnd.onClick = [this, on] {
      if (on) {
        disable();
      } else {
        enable(Until::Manual);
      }
    };
    addRow(*mode, dnd);
    Flex* card = addGroup(tr("control-center.detail.focus.until"));
    const auto add = [&](Until until, const char* key) {
      DetailRow row;
      row.glyph = "moon";
      row.title = tr(key);
      row.selected = on && current == until;
      row.onClick = [this, until] { enable(until); };
      addRow(*card, row);
    };
    add(Until::OneHour, "control-center.detail.focus.one-hour");
    add(Until::Tomorrow, "control-center.detail.focus.tomorrow");
    add(Until::Manual, "control-center.detail.focus.manual");
    if (on && (m_until == Until::OneHour || m_until == Until::Tomorrow)) {
      const std::time_t end = std::chrono::system_clock::to_time_t(m_endsAt);
      std::tm local{};
      localtime_r(&end, &local);
      addGroupFooter(*card, i18n::tr("control-center.detail.focus.active-until", "time",
                                     std::format("{:02}:{:02}", local.tm_hour, local.tm_min)));
    }
  }

  std::vector<DetailPage::FooterButton> FocusDetailPage::footerButtons() {
    return {{tr("control-center.detail.focus.settings"), [] { openSettings("notifications"); }}};
  }

  // ---- Search ----

  namespace {

    bool contains(std::string_view text, std::string_view query) {
      const auto lower = [](std::string_view v) {
        std::string out(v);
        std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
      };
      return lower(text).find(lower(query)) != std::string::npos;
    }

  } // namespace

  void SearchDetailPage::setActive(bool active) {
    if (active) {
      m_query.clear();
      if (m_input != nullptr) {
        m_input->setValue("");
      }
      m_focusPending = true;
    }
  }

  std::unique_ptr<Node> SearchDetailPage::createTop() {
    return ui::input({
        .out = &m_input,
        .placeholder = tr("control-center.detail.search.placeholder"),
        .clearButtonEnabled = true,
        .surfaceOpacity = panelCardOpacity(),
        .surfaceRole = ColorRole::Fill2,
        .onChange = [this](const std::string& text) {
          m_query = text;
        },
        .onSubmit = [this](const std::string&) {
          if (m_firstResult) {
            m_firstResult();
          }
        },
    });
  }

  void SearchDetailPage::sync() {
    if (m_focusPending && m_input != nullptr && m_input->inputArea() != nullptr) {
      m_focusPending = false;
      PanelManager::instance().inputDispatcher().setFocus(m_input->inputArea());
    }
  }

  std::string SearchDetailPage::structureKey() const {
    std::string key = m_query;
    if (m_network != nullptr && m_network->available() && !m_query.empty()) {
      for (const AccessPointInfo& ap : m_network->accessPoints()) {
        if (contains(ap.ssid, m_query)) {
          key += "|" + ap.ssid;
        }
      }
    }
    return key;
  }

  void SearchDetailPage::build() {
    m_firstResult = nullptr;
    struct Module {
      const char* title;
      const char* glyph;
      const char* page;
    };
    static constexpr Module kModules[] = {
        {"control-center.network.wifi", "wifi", "detail-wifi"},
        {"control-center.tabs.bluetooth", "bluetooth", "detail-bluetooth"},
        {"control-center.quick.focus", "moon", "detail-focus"},
        {"control-center.detail.sound.title", "volume", "detail-sound"},
        {"control-center.tabs.power", "battery-charging-2", "detail-power"},
    };
    Flex* modules = nullptr;
    for (const Module& m : kModules) {
      const std::string name = tr(m.title);
      if (!m_query.empty() && !contains(name, m_query)) {
        continue;
      }
      if (modules == nullptr) {
        modules = addGroup(tr("control-center.detail.search.modules"));
      }
      DetailRow row;
      row.glyph = m.glyph;
      row.title = name;
      row.onClick = [this, page = std::string(m.page)] { m_openTab(page); };
      if (!m_firstResult) {
        m_firstResult = row.onClick;
      }
      addRow(*modules, row);
    }
    Flex* networks = nullptr;
    if (!m_query.empty() && m_network != nullptr && m_network->available()) {
      for (const AccessPointInfo& ap : m_network->accessPoints()) {
        if (!contains(ap.ssid, m_query)) {
          continue;
        }
        if (networks == nullptr) {
          networks = addGroup(tr("control-center.detail.search.networks"));
        }
        DetailRow row;
        row.glyph = network_display::wifiGlyphForSignal(ap.strength);
        row.title = ap.ssid;
        if (ap.secured) {
          row.trailingGlyphs.push_back("lock");
        }
        row.onClick = [this] { m_openTab("detail-wifi"); };
        if (!m_firstResult) {
          m_firstResult = row.onClick;
        }
        addRow(*networks, row);
      }
    }
    if (modules == nullptr && networks == nullptr) {
      addEmpty("search", tr("control-center.detail.search.none"), "");
    }
  }

  // ---- Phone ----

  namespace {

    constexpr std::size_t kPhotoCount = 4;
    constexpr std::uint16_t kThumbnailSize = 160;

    std::string capitalized(std::string text) {
      if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
      }
      return text;
    }

    std::unique_ptr<Flex> chip(std::string_view glyphName, const std::string& text, float s, float opacity) {
      auto g = std::make_unique<Glyph>();
      g->setGlyph(glyphName);
      g->setGlyphSize(Tokens::spaceP12 * s);
      g->setColor(colorSpecFromRole(ColorRole::Label2));
      auto l = std::make_unique<Label>();
      l->setFontSize(Tokens::typeFootnote.size * s);
      l->setLineHeight(Tokens::typeFootnote.line * s);
      l->setColor(colorSpecFromRole(ColorRole::Label1));
      l->setText(text);
      auto c = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s}, std::move(g), std::move(l));
      c->setPadding(0.0F, Tokens::spaceP8 * s);
      c->setMinHeight(Tokens::spaceP24 * s);
      c->setRadius(Tokens::spaceP12 * s);
      c->setFill(colorSpecFromRole(ColorRole::Fill1, opacity));
      return c;
    }

    std::unique_ptr<Button> actionButton(std::string_view glyphName, const std::string& text, bool enabled, float s,
                                         std::function<void()> onClick) {
      auto b = std::make_unique<Button>();
      b->setVariant(ButtonVariant::Default);
      b->setGlyph(glyphName);
      b->setText(text);
      b->setControlHeight(Tokens::componentButtonHeightM * s);
      b->setRadius(Tokens::componentButtonRadius * s);
      b->setFlexGrow(1.0F);
      b->setEnabled(enabled);
      b->setOnClick(std::move(onClick));
      return b;
    }

  } // namespace

  const LinkDevice* PhoneDetailPage::device() const { return m_link != nullptr ? phone::primary(*m_link) : nullptr; }

  std::string PhoneDetailPage::headerTitle() const {
    const LinkDevice* phone = device();
    return phone != nullptr ? phone->name : std::string{};
  }

  std::vector<PhoneDetailPage::PhoneNotification> PhoneDetailPage::phoneNotifications(const std::string& deviceId) const {
    constexpr std::size_t kMaxShown = 3;
    std::vector<PhoneNotification> out;
    if (m_notifications == nullptr) {
      return out;
    }
    const std::string prefix = deviceId + "/";
    const auto& history = m_notifications->history();
    for (auto it = history.rbegin(); it != history.rend() && out.size() < kMaxShown; ++it) {
      const Notification& n = it->notification;
      if (!it->active || !n.group.has_value() || !n.group->starts_with(prefix)) {
        continue;
      }
      bool canReply = false;
      for (std::size_t i = 0; i < n.actions.size(); i += 2) {
        canReply = canReply || n.actions[i] == "inline-reply";
      }
      PhoneNotification shown{n.id, n.summary.empty() ? n.appName : n.summary, n.body, canReply, {}, {}};
      if (n.imageData.has_value()) {
        shown.image = std::make_shared<const NotificationImageData>(*n.imageData);
      }
      if (n.icon.has_value() && !n.icon->empty()) {
        shown.icons.push_back(n.icon->starts_with("file://") ? n.icon->substr(7) : *n.icon);
      }
      if (n.desktopEntry.has_value() && !n.desktopEntry->empty()) {
        shown.icons.push_back(*n.desktopEntry);
      }
      shown.icons.push_back(n.appName);
      out.push_back(std::move(shown));
    }
    return out;
  }

  std::string PhoneDetailPage::statusText(const LinkDevice& phone) const {
    if (m_link->paused(phone.id)) {
      return tr("control-center.detail.phone.paused");
    }
    if (m_link->connecting(phone.id)) {
      return tr("control-center.detail.phone.connecting");
    }
    return tr("bar.widgets.phone.not-connected");
  }

  std::unique_ptr<Flex> PhoneDetailPage::createHeaderActions() {
    const LinkDevice* phone = device();
    const bool on = phone != nullptr && !m_link->paused(phone->id);
    return headerToggle(m_toggle, on, contentScale(), [this](bool enabled) {
      const LinkDevice* current = device();
      if (current == nullptr) {
        return;
      }
      m_pendingOn = enabled;
      m_pendingSince = std::chrono::steady_clock::now();
      const std::weak_ptr<int> alive = m_alive;
      const auto done = [this, alive, enabled](bool ok) {
        if (alive.expired() || ok) {
          return;
        }
        m_pendingOn.reset();
        showMessage(tr(enabled ? "control-center.detail.phone.connect-failed" : "control-center.detail.phone.pause-failed"));
      };
      if (enabled) {
        m_link->connect(current->id, done);
      } else {
        m_link->disconnect(current->id, done);
      }
    });
  }

  void PhoneDetailPage::setActive(bool active) {
    m_active = active;
    if (!active) {
      m_photosAsked = false;
    }
  }

  void PhoneDetailPage::showMessage(std::string text) {
    constexpr auto kMessageTime = std::chrono::seconds(4);
    m_message = std::move(text);
    m_messageTimer.start(kMessageTime, [this] {
      m_message.clear();
      if (PanelManager::instance().isOpenPanel("control-center")) {
        requestRebuild();
      }
    });
    requestRebuild();
  }

  void PhoneDetailPage::sync() {
    const LinkDevice* phone = device();
    if (m_pendingOn.has_value()
        && (*m_pendingOn == !m_link->paused(phone != nullptr ? phone->id : std::string{})
            || std::chrono::steady_clock::now() - m_pendingSince > kPendingTimeout)) {
      m_pendingOn.reset();
    }
    if (m_toggle != nullptr && phone != nullptr) {
      m_toggle->setChecked(m_pendingOn.value_or(!m_link->paused(phone->id)));
    }
    if (phone == nullptr || !phone->connected) {
      m_photosAsked = false;
      m_thumbsReady = 0;
      return;
    }
    if (m_active && !m_photosAsked) {
      m_photosAsked = true;
      m_link->requestPhotos(phone->id, kPhotoCount);
    }
    m_thumbsReady = 0;
    if (const auto* photos = m_link->photos(phone->id); photos != nullptr) {
      for (std::size_t i = 0; i < photos->size() && i < kPhotoCount; ++i) {
        m_thumbsReady += m_link->photoThumbnail(phone->id, (*photos)[i].path, kThumbnailSize) != nullptr ? 1 : 0;
      }
    }
  }

  bool PhoneDetailPage::deferRebuild() const {
    return m_reply != nullptr
        && (!m_reply->value().empty() || PanelManager::instance().inputDispatcher().focusedArea() == m_reply->inputArea());
  }

  std::string PhoneDetailPage::structureKey() const {
    const LinkDevice* phone = device();
    if (phone == nullptr) {
      return "none";
    }
    std::string key = std::format("{}|{}|{}|{}|{}|{}", phone->id, phone->name, phone->connected, m_replyId,
                                  m_link->paused(phone->id), m_link->connecting(phone->id));
    key += std::format("|p{}|{}|{}", static_cast<int>(m_link->photosState(phone->id)), m_thumbsReady, m_message);
    if (const LinkStatus* status = m_link->status(phone->id); status != nullptr) {
      key += std::format("|{}|{}|{}", status->battery, status->charging, status->network);
    }
    if (const LinkControls* controls = m_link->controls(phone->id); controls != nullptr) {
      key += "|" + controls->ringer;
    }
    key += m_link->phoneRinging(phone->id) ? "|r" : "|-";
    key += std::format("|{}", m_link->notificationCount(phone->id));
    for (const PhoneNotification& n : phoneNotifications(phone->id)) {
      key += std::format("|{}", n.id);
    }
    return key;
  }

  void PhoneDetailPage::addReplyForm(Flex& card, std::uint32_t id) {
    const float s = contentScale();
    auto form = std::make_unique<Flex>();
    form->setDirection(FlexDirection::Horizontal);
    form->setAlign(FlexAlign::Center);
    form->setGap(Tokens::spaceP8 * s);
    form->setPadding(Tokens::spaceP4 * s, Tokens::componentListRowInset * s, Tokens::spaceP8 * s,
                     Tokens::componentListRowInset * s);
    const auto send = [this, id](const std::string& text) {
      if (text.empty() || m_notifications == nullptr) {
        return;
      }
      (void)m_notifications->invokeInlineReply(id, text);
      m_replyId = 0;
      requestRebuild();
    };
    form->addChild(ui::input({
        .out = &m_reply,
        .placeholder = tr("control-center.detail.phone.reply"),
        .surfaceOpacity = panelCardOpacity(),
        .surfaceRole = ColorRole::Fill2,
        .flexGrow = 1.0F,
        .onSubmit = send,
    }));
    auto sendButton = std::make_unique<Button>();
    sendButton->setVariant(ButtonVariant::Primary);
    sendButton->setText(tr("control-center.detail.phone.send"));
    sendButton->setControlHeight(Tokens::componentButtonHeightS * s);
    sendButton->setRadius(Tokens::componentButtonRadius * s);
    sendButton->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
    sendButton->setOnClick([this, send] {
      if (m_reply != nullptr) {
        send(m_reply->value());
      }
    });
    form->addChild(std::move(sendButton));
    card.addChild(std::move(form));
  }

  void PhoneDetailPage::build() {
    const LinkDevice* phone = device();
    if (phone == nullptr) {
      return;
    }
    const float s = contentScale();
    const std::string id = phone->id;
    const bool connected = phone->connected;

    auto chipRow = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
    if (!connected) {
      chipRow->addChild(chip("device-mobile-off", statusText(*phone), s, panelCardOpacity()));
    } else {
      const LinkStatus* status = m_link->status(id);
      if (status != nullptr) {
        chipRow->addChild(chip(phone::glyph(*phone, status), phone::batteryText(*status), s, panelCardOpacity()));
        chipRow->addChild(chip("wifi", capitalized(i18n::tr("bar.widgets.phone.network-" + status->network)), s, panelCardOpacity()));
      }
      const LinkControls* controls = m_link->controls(id);
      if (controls != nullptr && !controls->ringer.empty()) {
        chipRow->addChild(chip(phone::ringerGlyph(controls->ringer), i18n::tr("bar.widgets.phone.ringer-" + controls->ringer), s, panelCardOpacity()));
      }
    }
    addBlock(std::move(chipRow));

    // Three buttons do not fit one row at 360 without ellipsis: two on the first, one full width on the second.
    const bool ringing = m_link->phoneRinging(id);
    auto buttons = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
    buttons->addChild(ui::row(
        {.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s},
        actionButton("bell-ringing", i18n::tr(ringing ? "bar.widgets.phone.stop-ring" : "bar.widgets.phone.ring"), connected, s,
                     [this, id, ringing] {
                       m_link->ring(id, !ringing);
                       requestRebuild();
                     }),
        actionButton("clipboard", i18n::tr("bar.widgets.phone.send-clipboard"), connected, s,
                     [this, id] { m_link->shareClipboard(id); })));
    buttons->addChild(ui::row(
        {.align = FlexAlign::Stretch},
        actionButton("file-upload", tr("control-center.detail.phone.files"), connected, s, [this, id] {
          FileDialogOptions options;
          options.mode = FileDialogMode::Open;
          options.title = tr("control-center.detail.phone.files-title");
          (void)FileDialog::open(std::move(options), [this, id](std::optional<std::filesystem::path> picked) {
            if (picked.has_value() && m_link != nullptr) {
              (void)m_link->sendFiles(id, {picked->string()});
            }
          });
        })));
    addBlock(std::move(buttons));

    if (!m_message.empty()) {
      auto note = std::make_unique<Label>();
      note->setFontSize(Tokens::typeFootnote.size * s);
      note->setLineHeight(Tokens::typeFootnote.line * s);
      note->setColor(colorSpecFromRole(ColorRole::Label2));
      note->setText(m_message);
      auto inset = std::make_unique<Flex>();
      inset->setPadding(0.0F, 0.0F, 0.0F, (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
      inset->addChild(std::move(note));
      addBlock(std::move(inset));
    }
    if (!connected) {
      return;
    }
    const LinkPhotosState photosState = m_link->photosState(id);
    if (photosState == LinkPhotosState::Denied || photosState == LinkPhotosState::Unavailable) {
      auto note = std::make_unique<Label>();
      note->setFontSize(Tokens::typeFootnote.size * s);
      note->setLineHeight(Tokens::typeFootnote.line * s);
      note->setColor(colorSpecFromRole(ColorRole::Label2));
      note->setText(tr(photosState == LinkPhotosState::Denied ? "control-center.detail.phone.photos-denied"
                                                              : "control-center.detail.phone.photos-unavailable"));
      auto inset = std::make_unique<Flex>();
      inset->setPadding(0.0F, 0.0F, 0.0F, (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
      inset->addChild(std::move(note));
      addBlock(std::move(inset));
    }
    if (const auto* photos = m_link->photos(id); photos != nullptr && !photos->empty()) {
      auto section = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::componentListHeaderGap * s});
      auto header = std::make_unique<Label>();
      header->setFontSize(Tokens::typeFootnote.size * s);
      header->setLineHeight(Tokens::typeFootnote.line * s);
      header->setFontWeight(static_cast<FontWeight>(Style::glassWeight(600)));
      header->setColor(colorSpecFromRole(ColorRole::Label2));
      header->setText(tr("control-center.detail.phone.recent-photos"));
      auto inset = std::make_unique<Flex>();
      inset->setPadding(0.0F, 0.0F, 0.0F, (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
      inset->addChild(std::move(header));
      section->addChild(std::move(inset));
      auto thumbs = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s});
      for (std::size_t i = 0; i < photos->size() && i < kPhotoCount; ++i) {
        const LinkPhoto& photo = (*photos)[i];
        auto tile = std::make_unique<Button>();
        tile->setVariant(ButtonVariant::Default);
        tile->setPadding(0.0F);
        tile->setRadius(Tokens::componentButtonRadius * s);
        tile->setMinHeight(Tokens::spaceP64 * s);
        tile->setFlexGrow(1.0F);
        auto image = std::make_unique<Image>();
        image->setFit(ImageFit::Cover);
        image->setSize(Tokens::spaceP64 * 1.2F * s, Tokens::spaceP64 * s);
        image->setRadius(Tokens::componentButtonRadius * s);
        image->setParticipatesInLayout(false);
        if (auto thumb = m_link->photoThumbnail(id, photo.path, kThumbnailSize); thumb != nullptr) {
          bindImage(image.get(), {}, static_cast<int>(kThumbnailSize), std::move(thumb));
        }
        tile->addChild(std::move(image));
        tile->setOnClick([this, id, path = photo.path] {
          m_link->savePhoto(id, path, [this, alive = std::weak_ptr<int>(m_alive)](LinkSaveResult result) {
            if (alive.expired()) {
              return;
            }
            showMessage(tr(result == LinkSaveResult::Saved    ? "control-center.detail.phone.photo-saved"
                           : result == LinkSaveResult::Denied ? "control-center.detail.phone.photos-denied"
                                                              : "control-center.detail.phone.photo-failed"));
          });
        });
        thumbs->addChild(std::move(tile));
      }
      section->addChild(std::move(thumbs));
      addBlock(std::move(section));
    }
    const std::vector<PhoneNotification> shown = phoneNotifications(id);
    const std::uint32_t count = m_link->notificationCount(id);
    if (shown.empty() && count == 0) {
      Flex* empty = addGroup(tr("control-center.detail.phone.notifications-title"));
      auto title = std::make_unique<Label>();
      title->setFontSize(Tokens::typeBody.size * s);
      title->setLineHeight(Tokens::typeBody.line * s);
      title->setFontWeight(static_cast<FontWeight>(Style::glassWeight(600)));
      title->setColor(colorSpecFromRole(ColorRole::Label1));
      title->setText(tr("control-center.detail.phone.no-notifications"));
      auto body = std::make_unique<Label>();
      body->setFontSize(Tokens::typeFootnote.size * s);
      body->setLineHeight(Tokens::typeFootnote.line * s);
      body->setColor(colorSpecFromRole(ColorRole::Label2));
      body->setText(tr("control-center.detail.phone.no-notifications-body"));
      auto column = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center}, std::move(title), std::move(body));
      column->setPadding(Tokens::spaceP16 * s, 0.0F);
      empty->addChild(std::move(column));
      return;
    }
    Flex* card = addGroup(tr("control-center.detail.phone.notifications-title"));
    for (const PhoneNotification& n : shown) {
      DetailRow row;
      row.avatarImage = n.image;
      row.avatarIcons = n.icons;
      row.title = n.title;
      row.secondary = n.body;
      if (n.canReply) {
        row.trailing = tr("control-center.detail.phone.reply");
        row.onClick = [this, nid = n.id] {
          m_replyId = m_replyId == nid ? 0 : nid;
          requestRebuild();
        };
      }
      addRow(*card, row);
      if (n.canReply && m_replyId == n.id) {
        addReplyForm(*card, n.id);
      }
    }
  }

  std::vector<DetailPage::FooterButton> PhoneDetailPage::footerButtons() {
    return {{tr("control-center.detail.phone.open-link"), [] { LinkService::openApp(); }}};
  }

  // ---- Display ----

  DisplayDetailPage::DisplayDetailPage(BrightnessService* brightness, const ShortcutServices& shortcuts)
      : m_brightness(brightness), m_darkMode(ShortcutRegistry::create("dark_mode", shortcuts)) {
    if (shortcuts.nightLight != nullptr) {
      m_nightLight = ShortcutRegistry::create("nightlight", shortcuts);
    }
  }

  DisplayDetailPage::~DisplayDetailPage() = default;

  std::string DisplayDetailPage::structureKey() const {
    std::string key;
    if (m_brightness != nullptr) {
      for (const BrightnessDisplay& display : m_brightness->displays()) {
        if (display.controllable) {
          key += display.id + "|" + display.label + "|";
        }
      }
    }
    for (const Shortcut* shortcut : {m_darkMode.get(), m_nightLight.get()}) {
      if (shortcut != nullptr) {
        key += std::format("{}{}|{}", shortcut->id(), shortcut->active() ? 1 : 0, shortcut->displayLabel());
      }
    }
    return key;
  }

  void DisplayDetailPage::build() {
    const auto& displays = m_brightness != nullptr ? m_brightness->displays() : std::vector<BrightnessDisplay>{};
    Flex* sliders = nullptr;
    for (const BrightnessDisplay& display : displays) {
      if (!display.controllable) {
        continue;
      }
      if (sliders == nullptr) {
        sliders = addGroup(tr("control-center.detail.display.brightness"));
      }
      DetailSliderRow row;
      row.glyph = "sun";
      row.title = display.label;
      row.value = [this, id = display.id]() -> std::optional<float> {
        const BrightnessDisplay* current = m_brightness->findDisplay(id);
        return current != nullptr ? std::optional<float>(current->brightness) : std::nullopt;
      };
      row.onChange = [this, id = display.id](float value) { m_brightness->setBrightness(id, value); };
      addSliderRow(*sliders, row);
    }
    Flex* card = addGroup();
    for (Shortcut* shortcut : {m_darkMode.get(), m_nightLight.get()}) {
      if (shortcut == nullptr) {
        continue;
      }
      DetailRow row;
      row.glyph = std::string(shortcut->currentIcon());
      row.title = shortcut->defaultLabel();
      row.trailing = shortcut->active() ? tr("control-center.detail.display.on") : tr("control-center.detail.display.off");
      row.selected = shortcut->active();
      row.onClick = [this, shortcut] {
        shortcut->onClick();
        requestRebuild();
      };
      addRow(*card, row);
    }
  }

  std::vector<DetailPage::FooterButton> DisplayDetailPage::footerButtons() {
    return {{tr("control-center.detail.display.settings"), [] { openSettings("displays"); }}};
  }

  // ---- System ----

  SystemDetailPage::~SystemDetailPage() { retain(false); }

  void SystemDetailPage::setActive(bool active) {
    retain(active);
    if (active) {
      m_tick.startRepeating(std::chrono::seconds(1), [] { PanelManager::instance().requestUpdateOnly(); });
    } else {
      m_tick.stop();
    }
  }

  void SystemDetailPage::retain(bool on) {
    if (m_monitor == nullptr || on == m_retained) {
      return;
    }
    m_retained = on;
    if (on) {
      m_monitor->retainSampling();
      m_monitor->retainCpuTemp();
      m_monitor->retainGpuUsage();
      m_monitor->retainDiskPath("/");
      return;
    }
    m_monitor->releaseSampling();
    m_monitor->releaseCpuTemp();
    m_monitor->releaseGpuUsage();
    m_monitor->releaseDiskPath("/");
  }

  std::string SystemDetailPage::structureKey() const {
    if (m_monitor == nullptr) {
      return "unavailable";
    }
    const SystemStats stats = m_monitor->latest();
    return std::format("{}|{}|{}|{}|{}|{}|{}", std::lround(stats.cpuUsagePercent), std::lround(stats.ramUsagePercent),
                       std::lround(stats.gpuUsagePercent.value_or(-1.0)), std::lround(stats.cpuTempC.value_or(-1.0)),
                       std::lround(stats.netRxBytesPerSec / 1000.0), std::lround(stats.netTxBytesPerSec / 1000.0),
                       std::lround(m_monitor->diskUsagePercent("/")));
  }

  void SystemDetailPage::build() {
    if (m_monitor == nullptr) {
      addEmpty("activity-heartbeat", tr("control-center.detail.system.unavailable"), {});
      return;
    }
    const float s = contentScale();
    const SystemStats stats = m_monitor->latest();
    Flex* card = addGroup();
    auto rings = ui::row({.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = Tokens::spaceP32 * s,
                          .paddingV = Tokens::spaceP12 * s});
    const auto addRing = [&](float percent, const char* nameKey) {
      const float size = (Tokens::spaceP64 + Tokens::spaceP8) * s;
      auto cell = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s});
      auto stack = std::make_unique<Flex>();
      stack->setMinWidth(size);
      stack->setMinHeight(size);
      for (const bool value : {false, true}) {
        auto ring = std::make_unique<CountdownRing>();
        ring->setRingSize(size);
        ring->setThickness(Tokens::spaceP8 * s);
        ring->setProgress(value ? std::clamp(percent / 100.0F, 0.0F, 1.0F) : 1.0F);
        ring->setColor(colorSpecFromRole(value ? ColorRole::AccentText : ColorRole::Fill2));
        if (value) {
          ring->setParticipatesInLayout(false);
        }
        stack->addChild(std::move(ring));
      }
      cell->addChild(std::move(stack));
      cell->addChild(ui::label({.text = std::format("{} %", std::lround(percent)), .fontSize = Tokens::typeHeadline.size * s,
                                .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1)}));
      cell->addChild(ui::label({.text = tr(nameKey), .fontSize = Tokens::typeFootnote.size * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
      rings->addChild(std::move(cell));
    };
    addRing(static_cast<float>(stats.cpuUsagePercent), "control-center.detail.system.cpu");
    addRing(static_cast<float>(stats.ramUsagePercent), "control-center.detail.system.memory");
    if (stats.gpuUsagePercent.has_value()) {
      addRing(static_cast<float>(*stats.gpuUsagePercent), "control-center.detail.system.gpu");
    }
    card->addChild(std::move(rings));

    Flex* rows = addGroup();
    if (const auto disk = m_monitor->diskStats("/"); disk.has_value()) {
      const std::uint64_t total = m_monitor->diskTotalBytes("/");
      const std::uint64_t free = m_monitor->diskAvailBytes("/");
      addRow(*rows, {.glyph = "database",
                     .title = tr("control-center.detail.system.disk"),
                     .trailing = FormatUnits::formatDecimalBytesUsage(static_cast<double>(total - std::min(free, total)),
                                                                      static_cast<double>(total))});
    }
    addRow(*rows, {.glyph = "network",
                   .title = tr("control-center.detail.system.network"),
                   .trailing = std::format("{} ↓ · {} ↑", FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec),
                                           FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec))});
    if (stats.cpuTempC.has_value()) {
      addRow(*rows, {.glyph = "temperature",
                     .title = tr("control-center.detail.system.temperature"),
                     .trailing = std::format("{} °C", std::lround(*stats.cpuTempC))});
    }
  }

  std::vector<DetailPage::FooterButton> SystemDetailPage::footerButtons() {
    return {{tr("control-center.detail.system.activity"), [this] {
      if (m_ipc != nullptr) {
        (void)m_ipc->execute("activity-open");
      }
    }}};
  }

  // ---- Screen Time ----

  namespace {
    std::string formatScreenTime(std::chrono::seconds total) {
      const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(total).count();
      if (minutes < 60) {
        return i18n::tr("control-center.detail.screen-time.minutes", "minutes", static_cast<int>(minutes));
      }
      return i18n::tr("control-center.detail.screen-time.hours", "hours", static_cast<int>(minutes / 60), "minutes",
                      static_cast<int>(minutes % 60));
    }
  } // namespace

  void ScreenTimeDetailPage::setActive(bool active) {
    if (active) {
      // The service records once a minute; the page follows while shown.
      m_tick.startRepeating(std::chrono::seconds(30), [] { PanelManager::instance().requestUpdateOnly(); });
    } else {
      m_tick.stop();
    }
  }

  std::string ScreenTimeDetailPage::structureKey() const {
    if (m_screenTime == nullptr || !m_screenTime->enabled()) {
      return "off";
    }
    const ScreenTimeSnapshot week = m_screenTime->snapshot(14);
    std::string key = std::to_string(week.total.count());
    for (const auto& bucket : week.buckets) {
      key += "|" + std::to_string(bucket.count() / 60);
    }
    return key;
  }

  void ScreenTimeDetailPage::build() {
    if (m_screenTime == nullptr || !m_screenTime->enabled()) {
      addEmpty("hourglass", tr("control-center.detail.screen-time.off"), tr("control-center.detail.screen-time.off-body"));
      return;
    }
    const float s = contentScale();
    const ScreenTimeSnapshot fortnight = m_screenTime->snapshot(14);
    const std::size_t n = fortnight.buckets.size();
    const std::size_t split = n >= 7 ? n - 7 : 0;
    std::chrono::seconds thisWeek{0};
    std::chrono::seconds lastWeek{0};
    std::chrono::seconds peak{0};
    for (std::size_t i = 0; i < n; ++i) {
      (i >= split ? thisWeek : lastWeek) += fortnight.buckets[i];
      if (i >= split) {
        peak = std::max(peak, fortnight.buckets[i]);
      }
    }
    const std::chrono::seconds today = n > 0 ? fortnight.buckets.back() : std::chrono::seconds{0};

    Flex* card = addGroup();
    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP12 * s});
    auto top = ui::row({.align = FlexAlign::End, .justify = FlexJustify::SpaceBetween});
    top->addChild(ui::label({.text = formatScreenTime(today), .fontSize = Tokens::typeHero.size * s,
                             .fontWeight = FontWeight::Light, .color = colorSpecFromRole(ColorRole::Label1)}));
    top->addChild(ui::label({.text = tr("control-center.detail.screen-time.today"), .fontSize = Tokens::typeFootnote.size * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
    body->addChild(std::move(top));
    std::string average = i18n::tr("control-center.detail.screen-time.average", "time", formatScreenTime(thisWeek / 7));
    if (lastWeek.count() > 0) {
      const long change = std::lround(100.0 * static_cast<double>(thisWeek.count() - lastWeek.count())
                                      / static_cast<double>(lastWeek.count()));
      average += " · " + i18n::tr(change >= 0 ? "control-center.detail.screen-time.more" : "control-center.detail.screen-time.less",
                                  "percent", static_cast<int>(std::abs(change)));
    }
    body->addChild(ui::label({.text = average, .fontSize = Tokens::typeFootnote.size * s,
                              .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1, .ellipsize = TextEllipsize::End}));
    const float barsHeight = (Tokens::spaceP64 + Tokens::spaceP32) * s;
    auto bars = ui::row({.align = FlexAlign::End, .justify = FlexJustify::SpaceBetween, .paddingH = Tokens::spaceP8 * s});
    auto days = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .paddingH = Tokens::spaceP8 * s});
    for (std::size_t i = split; i < n; ++i) {
      const float share = peak.count() > 0 ? static_cast<float>(fortnight.buckets[i].count()) / static_cast<float>(peak.count()) : 0.0F;
      auto column = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::End, .minHeight = barsHeight});
      column->addChild(ui::box({.fill = colorSpecFromRole(i + 1 == n ? ColorRole::Accent : ColorRole::Fill3),
                                .radius = Tokens::spaceP4 * s, .width = (Tokens::spaceP24 + Tokens::spaceP4) * s,
                                .height = std::max(Tokens::spaceP4, share * barsHeight / s) * s}));
      bars->addChild(std::move(column));
      const std::string dayKey = i < fortnight.bucketDayKeys.size() ? fortnight.bucketDayKeys[i] : std::string();
      days->addChild(ui::label({.text = dayKey.empty() ? std::string() : ScreenTimeService::dayDisplayName(dayKey).substr(0, 1),
                                .fontSize = Tokens::typeCaption.size * s, .fontWeight = FontWeight::SemiBold,
                                .color = colorSpecFromRole(ColorRole::Label3), .width = (Tokens::spaceP24 + Tokens::spaceP4) * s,
                                .textAlign = TextAlign::Center}));
    }
    body->addChild(std::move(bars));
    body->addChild(std::move(days));
    card->addChild(std::move(body));

    const ScreenTimeSnapshot day = m_screenTime->snapshot(1);
    if (!day.apps.empty()) {
      Flex* apps = addGroup(tr("control-center.detail.screen-time.most-used"));
      for (std::size_t i = 0; i < std::min<std::size_t>(day.apps.size(), 3); ++i) {
        const ScreenTimeAppUsage& app = day.apps[i];
        addRow(*apps, {.title = app.displayName, .trailing = formatScreenTime(app.total), .avatarIcons = {app.appKey}});
      }
    }
  }

  std::vector<DetailPage::FooterButton> ScreenTimeDetailPage::footerButtons() { return {}; }

  // ---- Media ----

  namespace {
    std::string clockText(std::int64_t us) {
      const std::int64_t secs = std::max<std::int64_t>(0, us / 1'000'000);
      return std::format("{}:{:02}", secs / 60, secs % 60);
    }

    std::unique_ptr<Button> mediaButton(std::string_view glyphName, float box, float s, bool enabled,
                                        std::function<void()> onClick, bool filled = false) {
      auto b = std::make_unique<Button>();
      b->setVariant(filled ? ButtonVariant::Default : ButtonVariant::Ghost);
      b->setGlyph(glyphName);
      b->setGlyphSize(Tokens::sizeSymbol * (filled ? 1.25F : 1.0F) * s);
      b->setControlHeight(box * s);
      b->setMinWidth(box * s);
      b->setRadius(box * 0.5F * s);
      b->setEnabled(enabled);
      b->setOnClick(std::move(onClick));
      return b;
    }
  } // namespace

  void MediaDetailPage::setActive(bool active) {
    if (active) {
      // MprisService polls Position about once a second; the seek line follows while shown.
      m_tick.startRepeating(std::chrono::seconds(1), [] { PanelManager::instance().requestUpdateOnly(); });
    } else {
      m_tick.stop();
    }
  }

  std::string MediaDetailPage::structureKey() const {
    if (m_mpris == nullptr) {
      return "none";
    }
    std::string key;
    for (const MprisPlayerInfo& player : m_mpris->listPlayers()) {
      key += std::format("{}|{}|{}|{}|{}|{}|{};", player.busName, player.playbackStatus, player.title,
                         mpris::effectiveArtUrl(player), player.positionUs / 1'000'000, player.shuffle, player.loopStatus);
    }
    if (const auto active = m_mpris->activePlayer(); active.has_value()) {
      key += "*" + active->busName;
    }
    return key;
  }

  void MediaDetailPage::build() {
    const std::optional<MprisPlayerInfo> player = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
    if (!player.has_value()) {
      addEmpty("disc", tr("control-center.detail.media.empty"), tr("control-center.detail.media.empty-body"));
      return;
    }
    const float s = contentScale();
    const std::string bus = player->busName;
    Flex* card = addGroup();
    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8 * s, .padding = Tokens::spaceP8 * s});

    auto head = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s});
    const float art = Tokens::spaceP64 * s;
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Cover);
    image->setSize(art, art);
    image->setRadius(Tokens::spaceP8 * s);
    const std::string path = mpris::resolveArtworkSource(m_http, m_pendingArt, mpris::effectiveArtUrl(*player), [this] {
      requestRebuild();
      PanelManager::instance().refresh();
    }, m_alive);
    if (!path.empty()) {
      bindImage(image.get(), {path}, static_cast<int>(std::ceil(art * 2.0F)));
    }
    head->addChild(std::move(image));
    auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    const std::string artist = mpris::joinArtists(player->artists);
    text->addChild(ui::label({.text = player->title.empty() ? player->identity : player->title, .fontSize = Tokens::typeTitle3.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                              .ellipsize = TextEllipsize::End}));
    const std::string line = player->album.empty() ? artist : (artist.empty() ? player->album : artist + " · " + player->album);
    if (!line.empty()) {
      text->addChild(ui::label({.text = line, .fontSize = Tokens::typeFootnote.size * s, .color = colorSpecFromRole(ColorRole::Label2),
                                .maxLines = 1, .ellipsize = TextEllipsize::End}));
    }
    text->addChild(ui::label({.text = player->identity, .fontSize = Tokens::typeFootnote.size * s,
                              .color = colorSpecFromRole(ColorRole::Label3), .maxLines = 1, .ellipsize = TextEllipsize::End}));
    head->addChild(std::move(text));
    body->addChild(std::move(head));

    if (player->lengthUs > 0) {
      const float progress = std::clamp(static_cast<float>(player->positionUs) / static_cast<float>(player->lengthUs), 0.0F, 1.0F);
      auto track = ui::row({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Fill2),
                            .radius = Tokens::spaceP2 * s, .minHeight = Tokens::spaceP4 * s});
      auto fill = ui::box({.fill = colorSpecFromRole(ColorRole::Label1), .radius = Tokens::spaceP2 * s, .flexGrow = progress});
      track->addChild(std::move(fill));
      if (progress < 1.0F) {
        track->addChild(ui::box({.flexGrow = 1.0F - progress}));
      }
      body->addChild(std::move(track));
      auto times = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween});
      times->addChild(ui::label({.text = clockText(player->positionUs), .fontSize = Tokens::typeFootnote.size * s,
                                 .color = colorSpecFromRole(ColorRole::Label2)}));
      times->addChild(ui::label({.text = "-" + clockText(player->lengthUs - player->positionUs), .fontSize = Tokens::typeFootnote.size * s,
                                 .color = colorSpecFromRole(ColorRole::Label2)}));
      body->addChild(std::move(times));
    }

    auto controls = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP16 * s});
    const float small = Tokens::spaceP32;
    controls->addChild(mediaButton("arrows-shuffle", small, s, player->canControl,
                                   [this, bus, on = player->shuffle] { m_mpris->setShuffle(bus, !on); }));
    controls->addChild(mediaButton("player-skip-back", small, s, player->canGoPrevious, [this, bus] { m_mpris->previous(bus); }));
    controls->addChild(mediaButton(player->playbackStatus == "Playing" ? "player-pause" : "player-play", Tokens::spaceP48, s,
                                   player->canPlay || player->canPause, [this, bus] { m_mpris->playPause(bus); }, true));
    controls->addChild(mediaButton("player-skip-forward", small, s, player->canGoNext, [this, bus] { m_mpris->next(bus); }));
    controls->addChild(mediaButton(player->loopStatus == "Track" ? "repeat-once" : "repeat", small, s, player->canControl,
                                   [this, bus, loop = player->loopStatus] {
                                     m_mpris->setLoopStatus(bus, loop == "None" ? "Playlist" : (loop == "Playlist" ? "Track" : "None"));
                                   }));
    body->addChild(std::move(controls));
    card->addChild(std::move(body));

    const std::vector<MprisPlayerInfo> players = m_mpris->listPlayers();
    if (players.size() >= 2) {
      Flex* list = addGroup(tr("control-center.detail.media.players"));
      for (const MprisPlayerInfo& other : players) {
        DetailRow row;
        row.title = other.identity;
        row.secondary = other.title;
        row.trailing = other.playbackStatus == "Playing" ? tr("control-center.detail.media.playing") : tr("control-center.detail.media.paused");
        row.avatarIcons = {other.desktopEntry, other.identity};
        row.selected = other.busName == bus;
        row.onClick = [this, name = other.busName] {
          m_mpris->setPinnedPlayerPreference(name);
          requestRebuild();
        };
        addRow(*list, row);
      }
    }
  }

  // ---- Notifications ----

  namespace {
    std::string groupKeyOf(const Notification& n) { return n.group.has_value() ? "g:" + *n.group : "a:" + notificationDisplayAppName(n); }

    std::string groupLabelOf(const Notification& n) {
      if (!n.group.has_value()) {
        return notificationDisplayAppName(n);
      }
      std::string label = *n.group;
      if (const auto slash = label.rfind('/'); slash != std::string::npos) {
        label.replace(slash, 1, " · ");
      }
      return label;
    }

    std::string ageText(const Notification& n) {
      if (!n.receivedWallClock.has_value()) {
        return {};
      }
      const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - *n.receivedWallClock).count();
      if (minutes < 1) {
        return i18n::tr("control-center.detail.notifications.now");
      }
      if (minutes < 60) {
        return i18n::tr("control-center.detail.notifications.minutes", "count", static_cast<int>(minutes));
      }
      if (minutes < 24 * 60) {
        return i18n::tr("control-center.detail.notifications.hours", "count", static_cast<int>(minutes / 60));
      }
      return i18n::tr("control-center.detail.notifications.days", "count", static_cast<int>(minutes / (24 * 60)));
    }
  } // namespace

  void NotificationsDetailPage::setActive(bool active) {
    if (active && m_notifications != nullptr) {
      m_notifications->markNotificationHistorySeen();
    }
    if (!active) {
      m_expanded.clear();
    }
  }

  std::string NotificationsDetailPage::structureKey() const {
    if (m_notifications == nullptr) {
      return "none";
    }
    std::string key;
    for (const NotificationHistoryEntry& entry : m_notifications->history()) {
      key += std::format("{}:{};", entry.notification.id, ageText(entry.notification));
    }
    for (const std::string& group : m_expanded) {
      key += "+" + group;
    }
    return key;
  }

  void NotificationsDetailPage::build() {
    if (m_notifications == nullptr || m_notifications->history().empty()) {
      setFooterVisible(false);
      addEmpty("bell", tr("control-center.detail.notifications.empty"), {});
      return;
    }
    setFooterVisible(true);
    // Newest first, grouped by app (or the x-andromeda-group hint) in order of each group's newest notification.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const Notification*>> groups;
    const auto& history = m_notifications->history();
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      const std::string key = groupKeyOf(it->notification);
      auto& list = groups[key];
      if (list.empty()) {
        order.push_back(key);
      }
      list.push_back(&it->notification);
    }
    for (const std::string& key : order) {
      const auto& list = groups[key];
      std::vector<std::uint32_t> ids;
      for (const Notification* n : list) {
        ids.push_back(n->id);
      }
      Flex* card = addGroup(groupLabelOf(*list.front()), tr("control-center.detail.notifications.clear"), [this, ids] {
        for (const std::uint32_t id : ids) {
          m_notifications->removeHistoryEntry(id);
        }
        requestRebuild();
      });
      const bool expanded = m_expanded.contains(key);
      const std::size_t shown = expanded ? list.size() : std::min<std::size_t>(list.size(), 2);
      for (std::size_t i = 0; i < shown; ++i) {
        const Notification& n = *list[i];
        DetailRow row;
        row.title = n.summary.empty() ? notificationDisplayAppName(n) : n.summary;
        row.secondary = n.body;
        row.trailing = ageText(n);
        if (n.imageData.has_value()) {
          row.avatarImage = std::make_shared<const NotificationImageData>(*n.imageData);
        }
        if (n.icon.has_value()) {
          row.avatarIcons.push_back(*n.icon);
        }
        if (n.desktopEntry.has_value()) {
          row.avatarIcons.push_back(*n.desktopEntry);
        }
        if (row.avatarImage == nullptr && row.avatarIcons.empty()) {
          row.glyph = "bell";
        }
        row.onClick = [this, id = n.id] {
          if (!m_notifications->canInvokeActions(id)) {
            return;
          }
          const std::string token = m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
          (void)m_notifications->invokeAction(id, "default", token, true);
          PanelManager::instance().closePanel();
        };
        // The app's first named action (Reply, Open) as the row button; "default" is the row click above.
        if (m_notifications->canInvokeActions(n.id)) {
          for (std::size_t a = 0; a + 1 < n.actions.size(); a += 2) {
            if (n.actions[a] != "default" && !n.actions[a + 1].empty()) {
              row.action = n.actions[a + 1];
              row.onAction = [this, id = n.id, key = n.actions[a]] {
                const std::string token =
                    m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
                (void)m_notifications->invokeAction(id, key, token, true);
                PanelManager::instance().closePanel();
              };
              break;
            }
          }
        }
        addRow(*card, row);
      }
      if (list.size() > 2) {
        addShowAll(*card, expanded, [this, key, expanded] {
          if (expanded) {
            m_expanded.erase(key);
          } else {
            m_expanded.insert(key);
          }
          requestRebuild();
        });
      }
    }
  }

  std::vector<DetailPage::FooterButton> NotificationsDetailPage::footerButtons() {
    return {{tr("control-center.detail.notifications.clear-all"), [this] {
               m_notifications->clearHistory();
               requestRebuild();
             }}};
  }

} // namespace control_center
