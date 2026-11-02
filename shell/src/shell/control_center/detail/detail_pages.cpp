#include "shell/control_center/detail/detail_pages.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "pipewire/pipewire_service.h"
#include "shell/control_center/tabs/bluetooth_tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <format>
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
    std::string key = std::format("{}|{}|{}|{}|{}|{}", state.wirelessEnabled, state.resolving, state.ssid,
                                  m_showAll, m_passwordSsid, m_error);
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
      row.secondary = state.resolving ? tr("control-center.quick.connecting") : std::string{};
      row.selected = true;
      row.trailingGlyphs = {};
      if (active.secured) {
        row.trailingGlyphs.push_back("lock");
      }
      addRow(*card, row);
    }

    std::vector<const AccessPointInfo*> others;
    for (const AccessPointInfo& ap : aps) {
      if (!ap.active) {
        others.push_back(&ap);
      }
    }
    if (others.empty()) {
      if (aps.empty()) {
        addEmpty("wifi", tr("control-center.detail.wifi.none"), "");
      }
      return;
    }
    Flex* card = addGroup(tr("control-center.detail.wifi.other-networks"));
    const std::size_t shown = m_showAll ? others.size() : std::min(others.size(), kCollapsedRows);
    for (std::size_t i = 0; i < shown; ++i) {
      const AccessPointInfo ap = *others[i];
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
        continue;
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
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .surfaceRole = ColorRole::SolidRegular,
          .flexGrow = 1.0F,
          .onSubmit = join,
      }));
      auto joinButton = std::make_unique<Button>();
      joinButton->setVariant(ButtonVariant::Primary);
      joinButton->setText(tr("control-center.network.join"));
      joinButton->setControlHeight(Tokens::componentButtonHeightS * s);
      joinButton->setRadius(Tokens::componentButtonRadius * s);
      joinButton->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
      joinButton->setOnClick([this, join] {
        if (m_password != nullptr) {
          join(m_password->value());
        }
      });
      form->addChild(std::move(joinButton));
      card->addChild(std::move(form));
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
      m_toggle->setEnabled(state.adapterPresent);
      m_toggle->setChecked(state.powered);
    }
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
    std::string key = std::format("{}|{}|{}|{}", state.powered, state.discovering, m_showNearby, m_showAllPaired);
    for (const BluetoothDeviceInfo& d : m_bluetooth->devices()) {
      key += std::format("|{}:{}:{}:{}:{}:{}:{}", d.path, d.alias, d.paired, d.connected, d.connecting, d.hasBattery,
                         d.batteryPercent);
    }
    return key;
  }

  void BluetoothDetailPage::build() {
    if (m_bluetooth == nullptr || !m_bluetooth->state().adapterPresent) {
      addEmpty("bluetooth-off", tr("control-center.detail.bluetooth.unavailable"),
               tr("control-center.detail.bluetooth.unavailable-body"));
      return;
    }
    if (!m_bluetooth->state().powered) {
      addEmpty("bluetooth-off", tr("control-center.detail.bluetooth.off"), tr("control-center.detail.bluetooth.off-body"));
      return;
    }
    const auto deviceRow = [this](const BluetoothDeviceInfo& d, std::function<void()> click) {
      DetailRow row;
      row.glyph = bluetoothDeviceGlyph(d.kind);
      row.title = d.alias;
      if (d.connecting) {
        row.secondary = tr("control-center.quick.connecting");
      }
      if (d.connected && d.hasBattery) {
        row.trailing = std::format("{}%", d.batteryPercent);
      }
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
    return key;
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
  }

  std::vector<DetailPage::FooterButton> SoundDetailPage::footerButtons() {
    return {{tr("control-center.detail.sound.settings"), [] { openSettings("sound"); }}};
  }

  // ---- Power ----

  std::string PowerDetailPage::structureKey() const {
    if (m_profiles == nullptr || m_profiles->profiles().empty()) {
      return "unavailable";
    }
    std::string key = m_profiles->activeProfile();
    for (const std::string& p : m_profiles->profiles()) {
      key += "|" + p;
    }
    if (m_upower != nullptr) {
      if (const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery(); battery != nullptr) {
        key += std::format("|{}", battery->healthPercent().value_or(-1.0));
      }
    }
    return key;
  }

  void PowerDetailPage::build() {
    if (m_profiles == nullptr || m_profiles->profiles().empty()) {
      addEmpty("battery-off", tr("control-center.detail.power.unavailable"), tr("control-center.detail.power.unavailable-body"));
      return;
    }
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
          addGroupFooter(*card, i18n::tr("control-center.detail.power.health", "percent", static_cast<int>(std::lround(*health))));
        }
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
    m_notifications->setDoNotDisturb(true);
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
        .surfaceRole = ColorRole::SolidRegular,
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

} // namespace control_center
