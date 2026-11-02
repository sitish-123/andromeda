#include "shell/settings/settings_content_bluetooth.h"
#include "shell/settings/settings_content_network.h"
#include "shell/settings/settings_content_sound.h"
#include "shell/settings/settings_content_users.h"
#include "shell/settings/settings_content_wifi.h"
#include "pipewire/pipewire_service.h"
#include "shell/settings/settings_window.h"
#include "ui/controls/slider.h"

void SettingsWindow::addWifiContent(float scale) {
  if (m_selectedSection != "wifi") {
    return;
  }
  settings::addSettingsWifi(
      *m_contentContainer,
      settings::SettingsWifiContext{
          .scale = scale,
          .network = m_networkProvider ? m_networkProvider() : nullptr,
          .detail = m_detail,
          .joinSsid = m_wifiJoinSsid,
          .password = [this] { return m_wifiPassword; },
          .openDetail = [this](std::string detail) { openDetail(std::move(detail)); },
          .setJoinSsid =
              [this](std::string ssid) {
                m_wifiJoinSsid = std::move(ssid);
                m_wifiPassword.clear();
                requestContentRebuild();
              },
          .setPassword = [this](const std::string& text) { m_wifiPassword = text; },
          .back = [this] { leaveDetail(); },
      }
  );
}

void SettingsWindow::onConnectivityChanged() {
  if (!isOpen() || !m_wifiJoinSsid.empty()) {
    return;
  }
  if (m_selectedSection == "wifi" || m_selectedSection == "bluetooth" || m_selectedSection == "network") {
    requestContentRebuild();
  }
}

void SettingsWindow::addBluetoothContent(float scale) {
  if (m_selectedSection != "bluetooth") {
    return;
  }
  settings::addSettingsBluetooth(
      *m_contentContainer,
      settings::SettingsBluetoothContext{
          .scale = scale,
          .bluetooth = m_bluetoothProvider ? m_bluetoothProvider() : nullptr,
          .detail = m_detail,
          .openDetail = [this](std::string detail) { openDetail(std::move(detail)); },
          .back = [this] { leaveDetail(); },
      }
  );
}

void SettingsWindow::addNetworkContent(float scale) {
  if (m_selectedSection != "network") {
    return;
  }
  settings::addSettingsNetwork(
      *m_contentContainer,
      settings::SettingsNetworkContext{
          .scale = scale,
          .network = m_networkProvider ? m_networkProvider() : nullptr,
          .openPage = [this](std::string page) { navigateTo(std::move(page), {}, false, {}, {}); },
      }
  );
}

void SettingsWindow::addSoundContent(float scale) {
  m_soundOutputSlider = nullptr;
  m_soundInputSlider = nullptr;
  if (m_selectedSection != "sound") {
    return;
  }
  PipeWireService* audio = m_audioProvider ? m_audioProvider() : nullptr;
  m_soundSignature = settings::soundDeviceSignature(audio);
  settings::addSettingsSound(
      *m_contentContainer,
      settings::SettingsSoundContext{
          .scale = scale, .audio = audio, .outputSlider = &m_soundOutputSlider, .inputSlider = &m_soundInputSlider
      }
  );
}

void SettingsWindow::onAudioChanged() {
  if (!isOpen() || m_selectedSection != "sound") {
    return;
  }
  PipeWireService* audio = m_audioProvider ? m_audioProvider() : nullptr;
  if (settings::soundDeviceSignature(audio) != m_soundSignature) {
    requestContentRebuild();
    return;
  }
  const auto follow = [](Slider* slider, const AudioNode* node) {
    if (slider != nullptr && node != nullptr && !slider->dragging()) {
      slider->setValue(node->volume);
    }
  };
  follow(m_soundOutputSlider, audio->defaultSink());
  follow(m_soundInputSlider, audio->defaultSource());
}

void SettingsWindow::addUsersContent(float scale) {
  if (m_selectedSection == "users") {
    settings::addSettingsUsers(*m_contentContainer, scale);
  }
}

void SettingsWindow::leaveDetail() {
  // The detail is gone (forgotten): Forward must not return to it.
  navigateHistory(-1);
  m_navHistory.resize(m_navIndex + 1);
}
