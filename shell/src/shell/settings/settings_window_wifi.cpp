#include "shell/settings/settings_content_bluetooth.h"
#include "shell/settings/settings_content_network.h"
#include "shell/settings/settings_content_sound.h"
#include "shell/settings/settings_content_users.h"
#include "shell/settings/settings_content_wifi.h"
#include "dbus/accounts/accounts_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/settings/settings_window.h"
#include "ui/controls/slider.h"

#include <cstring>

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
  if (m_selectedSection != "users") {
    return;
  }
  settings::addSettingsUsers(
      *m_contentContainer,
      settings::SettingsUsersContext{
          .scale = scale,
          .detail = m_detail,
          .draft = m_accounts != nullptr ? &m_newUser : nullptr,
          // A create in flight keeps its draft: the reply writes to it, and a second create must wait.
          .openDetail = [this](std::string detail) {
            if (!m_newUser.busy) {
              clearNewUser();
            }
            openDetail(std::move(detail));
          },
          .create = [this] { createNewUser(); },
          .back = [this] {
            if (!m_newUser.busy) {
              clearNewUser();
            }
            leaveDetail();
          },
          .rebuild = [this] { requestContentRebuild(); },
      }
  );
}

void SettingsWindow::clearNewUser() {
  explicit_bzero(m_newUser.password.data(), m_newUser.password.size());
  explicit_bzero(m_newUser.verify.data(), m_newUser.verify.size());
  m_newUser = {};
}

void SettingsWindow::createNewUser() {
  settings::NewUserDraft& draft = m_newUser;
  if (draft.busy || m_accounts == nullptr) {
    return;
  }
  const std::string login = draft.login.empty() ? settings::loginFromName(draft.fullName) : draft.login;
  draft.error.clear();
  if (!settings::validLogin(login)) {
    draft.error = i18n::tr("settings.users.error-login");
  } else if (draft.password.empty()) {
    draft.error = i18n::tr("settings.users.error-password");
  } else if (draft.password != draft.verify) {
    draft.error = i18n::tr("settings.users.error-mismatch");
  }
  if (draft.error.empty()) {
    draft.busy = true;
    m_accounts->createUser(login, draft.fullName, draft.admin, draft.password, [this](std::string error) {
      m_newUser.busy = false;
      if (error.empty()) {
        clearNewUser();
        if (m_detail.starts_with("users:")) {
          leaveDetail();
        }
      } else {
        m_newUser.error = std::move(error);
      }
      if (isOpen()) {
        requestContentRebuild();
      }
    });
  }
  requestContentRebuild();
}

void SettingsWindow::leaveDetail() {
  // The detail is gone (forgotten): Forward must not return to it.
  navigateHistory(-1);
  m_navHistory.resize(m_navIndex + 1);
}
