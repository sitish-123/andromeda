#include "shell/settings/settings_content_about.h"
#include "shell/settings/settings_content_bluetooth.h"
#include "shell/settings/settings_alert.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "dbus/system_interactive_call.h"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <utility>
#include <vector>
#include "shell/settings/settings_content_network.h"
#include "shell/settings/settings_content_power.h"
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
          .joinError = m_wifiJoinError,
          .password = [this] { return m_wifiPassword; },
          .identity = [this] { return m_wifiIdentity; },
          .openDetail = [this](std::string detail) { openDetail(std::move(detail)); },
          .setJoinSsid =
              [this](std::string ssid) {
                m_wifiJoinSsid = std::move(ssid);
                m_wifiPassword.clear();
                m_wifiIdentity.clear();
                m_wifiJoinError.clear();
                requestContentRebuild();
              },
          .setPassword = [this](const std::string& text) { m_wifiPassword = text; },
          .setIdentity = [this](const std::string& text) { m_wifiIdentity = text; },
          .setJoinError =
              [this](std::string error) {
                m_wifiJoinError = std::move(error);
                requestContentRebuild();
              },
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
  m_soundProgramSliders.clear();
  settings::addSettingsSound(
      *m_contentContainer,
      settings::SettingsSoundContext{
          .scale = scale,
          .audio = audio,
          .outputSlider = &m_soundOutputSlider,
          .inputSlider = &m_soundInputSlider,
          .programSliders = &m_soundProgramSliders,
      }
  );
}

void SettingsWindow::onAudioChanged() {
  if (!isOpen() || m_selectedSection != "sound") {
    return;
  }
  PipeWireService* audio = m_audioProvider ? m_audioProvider() : nullptr;
  const auto dragging = [](const Slider* slider) { return slider != nullptr && slider->dragging(); };
  const bool held = dragging(m_soundOutputSlider) || dragging(m_soundInputSlider)
      || std::ranges::any_of(m_soundProgramSliders, [&](const auto& entry) { return dragging(entry.second); });
  // A rebuild would end the drag; the next audio change after release rebuilds.
  if (!held && settings::soundDeviceSignature(audio) != m_soundSignature) {
    requestContentRebuild();
    return;
  }
  const auto follow = [&](Slider* slider, const AudioNode* node) {
    if (slider != nullptr && node != nullptr && !dragging(slider)) {
      slider->setValue(node->volume);
    }
  };
  follow(m_soundOutputSlider, audio->defaultSink());
  follow(m_soundInputSlider, audio->defaultSource());
  for (const auto& [id, slider] : m_soundProgramSliders) {
    const auto& programs = audio->state().programOutputs;
    const auto it = std::ranges::find(programs, id, &AudioNode::id);
    follow(slider, it != programs.end() ? &*it : nullptr);
  }
}

void SettingsWindow::addUsersContent(float scale) {
  if (m_selectedSection != "users") {
    return;
  }
  const auto deleteUser = m_accounts == nullptr ? std::function<void(unsigned)>{} : [this](unsigned uid) {
    m_userDeleteBusy = true;
    requestContentRebuild();
    m_accounts->deleteUser(uid, [this](std::string error) {
      m_userDeleteBusy = false;
      m_userDeleteError = std::move(error);
      if (m_userDeleteError.empty() && m_detail.starts_with("user:")) {
        leaveDetail();
      }
      if (isOpen()) {
        requestContentRebuild();
      }
    });
  };
  settings::addSettingsUsers(
      *m_contentContainer,
      settings::SettingsUsersContext{
          .scale = scale,
          .detail = m_detail,
          .draft = m_accounts != nullptr ? &m_newUser : nullptr,
          // A create in flight keeps its draft: the reply writes to it, and a second create must wait.
          .deleteBusy = m_userDeleteBusy,
          .deleteError = m_userDeleteError,
          .confirmDelete =
              [this, scale, deleteUser](unsigned uid, std::string login) {
                m_userDeleteError.clear();
                settings::showSettingsAlert(
                    m_modalHost,
                    settings::SettingsAlertRequest{
                        .title = i18n::tr("settings.users.delete-confirm", "name", login),
                        .body = i18n::tr("settings.users.delete-body"),
                        .buttons = {
                            {.text = i18n::tr("common.actions.cancel")},
                            {.text = i18n::tr("settings.users.delete"),
                             .variant = ButtonVariant::Destructive,
                             .onClick = [deleteUser, uid] { deleteUser(uid); }},
                        },
                        // Cancel is the default for a destructive action, so Enter never deletes.
                        .defaultIndex = 0,
                        .scale = scale,
                    }
                );
              },
          .deleteUser = deleteUser,
          .openDetail = [this](std::string detail) {
            if (!m_newUser.busy) {
              clearNewUser();
            }
            if (!m_userDeleteBusy) {
              m_userDeleteError.clear();
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

void SettingsWindow::addPowerContent(float scale) {
  if (m_selectedSection != "power" || !m_detail.empty()) {
    return;
  }
  settings::addSettingsPower(
      *m_contentContainer,
      settings::SettingsPowerContext{
          .scale = scale,
          .upower = m_upower,
          .profiles = m_powerProfilesProvider ? m_powerProfilesProvider() : nullptr,
          .powerConfig = m_powerConfigProvider ? m_powerConfigProvider() : nullptr,
      }
  );
}

void SettingsWindow::onPowerChanged() {
  if (isOpen() && m_selectedSection == "power" && m_searchQuery.empty() && !m_expertOpen) {
    requestContentRebuild();
  }
}

void SettingsWindow::addAboutContent(float scale) {
  if (m_selectedSection != "about" || !m_detail.empty()) {
    return;
  }
  settings::addSettingsAbout(
      *m_contentContainer,
      settings::SettingsAboutContext{
          .scale = scale,
          .renaming = m_renaming,
          .renameError = m_renameError,
          .updates = m_updateCount,
          .checkUpdates = !process::commandExists("checkupdates") ? std::function<void()>{} : [this] { checkUpdates(); },
          .installUpdates = [this] {
            // The package manager asks for the password in the terminal; the window stays open to show the result.
            const std::string update = process::commandExists("yay") ? "yay -Syu" : "sudo pacman -Syu";
            const std::string script = update + "; printf '\\nPress Enter to close. '; read -r _";
            for (const auto& [terminal, flag] : std::initializer_list<std::pair<const char*, const char*>>{
                     {"xdg-terminal-exec", ""}, {"foot", ""}, {"kitty", ""}, {"alacritty", "-e"}}) {
              if (process::commandExists(terminal)) {
                std::vector<std::string> args{terminal};
                if (flag[0] != '\0') {
                  args.emplace_back(flag);
                }
                args.insert(args.end(), {"sh", "-c", script});
                // The count is stale once the update runs; the user checks again.
                m_updateCount = process::runAsync(args) ? -1 : -4;
                requestContentRebuild();
                return;
              }
            }
            m_updateCount = -4;
            requestContentRebuild();
          },
          .rename =
              [this](std::string name) {
                const std::string hostname = settings::hostnameFromName(name);
                m_renaming = true;
                m_renameError.clear();
                requestContentRebuild();
                const auto finish = [this](std::string error) {
                  m_renaming = false;
                  m_renameError = std::move(error);
                  if (isOpen()) {
                    requestContentRebuild();
                  }
                };
                // The pretty name keeps the user's spelling; the static hostname is its DNS-safe form.
                dbus::callInteractive(
                    "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
                    "SetPrettyHostname", g_variant_new("(sb)", name.c_str(), TRUE),
                    [finish, hostname](GVariant* done, std::string error) {
                      if (done == nullptr || hostname.empty()) {
                        finish(std::move(error));
                        return;
                      }
                      dbus::callInteractive(
                          "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
                          "SetStaticHostname", g_variant_new("(sb)", hostname.c_str(), TRUE),
                          [finish](GVariant*, std::string staticError) { finish(std::move(staticError)); }
                      );
                    }
                );
              },
      }
  );
}

void SettingsWindow::checkUpdates() {
  if (m_updateCount == -2) {
    return;
  }
  m_updateCount = -2;
  requestContentRebuild();
  // checkupdates: exit 0 with one line per update, 2 when there are none, 1 on failure.
  const bool started = process::runAsync(
      std::vector<std::string>{"checkupdates"},
      process::RunCallbacks{
          .onExit =
              [this, alive = std::weak_ptr<void>(m_aliveGuard)](process::RunResult result) {
                // Runs on the process worker thread: count here, apply on the main loop.
                const int count = result.exitCode == 2 ? 0
                    : result.exitCode == 0
                    ? static_cast<int>(std::ranges::count(result.out, '\n')) + (result.out.empty() ? 0 : 1)
                    : -3;
                DeferredCall::callLater([this, alive, count] {
                  if (alive.expired()) {
                    return;
                  }
                  m_updateCount = count;
                  if (isOpen()) {
                    requestContentRebuild();
                  }
                });
              },
      },
      process::RunOptions{.timeout = std::chrono::minutes(2)}
  );
  if (!started) {
    m_updateCount = -3;
  }
}
