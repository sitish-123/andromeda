#include "shell/settings/settings_content_about.h"
#include "clipboard/clipboard_service.h"
#include "dbus/andromeda/software_update_service.h"
#include "net/url_open.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "shell/settings/settings_content_bluetooth.h"
#include "shell/settings/settings_alert.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "core/shared_worker.h"
#include "core/process/process.h"
#include "dbus/system_interactive_call.h"

#include "dbus/system_bus.h"

#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <utility>
#include <vector>
#include "shell/settings/settings_content_network.h"
#include "shell/settings/settings_content_power.h"
#include "shell/settings/settings_content_workspaces.h"
#include "shell/settings/settings_content_sound.h"
#include "shell/settings/settings_content_users.h"
#include "shell/settings/settings_content_wifi.h"
#include "dbus/accounts/accounts_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "shell/settings/settings_window.h"
#include "system/display_manager.h"
#include "ui/controls/slider.h"

#include <cstring>
#include <map>

namespace {

  constexpr auto kHostnameInterface = "org.freedesktop.hostname1";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  using VariantMap = std::map<std::string, sdbus::Variant>;

  std::map<std::string, std::string> stringProperties(const VariantMap& properties) {
    std::map<std::string, std::string> out;
    for (const auto& [key, value] : properties) {
      if (value.containsValueOfType<std::string>()) {
        out[key] = value.get<std::string>();
      }
    }
    return out;
  }

} // namespace

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
    if (const auto next = settings::speakerTransition(m_speakerPhase, settings::SpeakerEvent::Close)) {
      m_speakerPhase = *next;
    }
    return;
  }
  if (m_speakerPhase == settings::SpeakerPhase::Unloaded) {
    runSpeaker({"status"}, settings::SpeakerEvent::Open);
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
          .speakerPhase = m_speakerPhase,
          .speakerStatus = m_speakerStatus ? &*m_speakerStatus : nullptr,
          .speakerError = m_speakerError,
          .speakerFineTuneOpen = m_speakerFineTuneOpen,
          .runSpeaker =
              [this](std::vector<std::string> args) {
                const auto event =
                    args.front() == "import-windows" ? settings::SpeakerEvent::Import : settings::SpeakerEvent::Apply;
                runSpeaker(std::move(args), event);
              },
          .toggleSpeakerFineTune =
              [this] {
                m_speakerFineTuneOpen = !m_speakerFineTuneOpen;
                requestContentRebuild();
              },
      }
  );
}

void SettingsWindow::reloadSpeakerStatus() {
  // A running command reads the status itself when it ends.
  if (const auto next = settings::speakerTransition(m_speakerPhase, settings::SpeakerEvent::Close)) {
    m_speakerPhase = *next;
    if (isOpen()) {
      requestContentRebuild();
    }
  }
}

// One andromeda-sound command at a time: speakerTransition rejects a second one.
void SettingsWindow::runSpeaker(std::vector<std::string> args, settings::SpeakerEvent start) {
  const auto next = settings::speakerTransition(m_speakerPhase, start);
  if (!next) {
    // Wheel ticks and keys apply faster than the chain restarts: the last one runs after the current command.
    if (start == settings::SpeakerEvent::Apply && settings::speakerBusy(m_speakerPhase)) {
      m_speakerQueuedApply = std::move(args);
    }
    return;
  }
  m_speakerPhase = *next;
  startSpeakerCommand(std::move(args));
  requestContentRebuild();
}

void SettingsWindow::startSpeakerCommand(std::vector<std::string> args) {
  const bool status = args.front() == "status";
  const bool import = args.front() == "import-windows";
  args.insert(args.begin(), "andromeda-sound");
  auto finish = [this, alive = std::weak_ptr<void>(m_aliveGuard), status, import](process::RunResult result) {
    std::optional<settings::SpeakerStatus> parsed =
        status && result ? settings::parseSpeakerStatus(result.out) : std::nullopt;
    const bool ok = static_cast<bool>(result);
    DeferredCall::callLater([this, alive, status, import, ok, parsed = std::move(parsed),
                             err = std::move(result.err)]() mutable {
      if (!alive.expired()) {
        onSpeakerCommandDone(status, import, ok, std::move(parsed), err);
      }
    });
  };
  // Import can wait for the user's password (udisks mount); apply restarts the filter chain.
  const auto timeout = import ? std::chrono::seconds(120) : std::chrono::seconds(15);
  if (!process::runAsync(args, process::RunCallbacks{.onExit = std::move(finish)},
                         process::RunOptions{.timeout = timeout})) {
    onSpeakerCommandDone(status, import, false, std::nullopt, {});
  }
}

void SettingsWindow::onSpeakerCommandDone(bool status, bool import, bool ok,
                                          std::optional<settings::SpeakerStatus> parsed, const std::string& err) {
  using E = settings::SpeakerEvent;
  E event = E::CommandDone;
  if (status) {
    event = parsed ? E::StatusOk : E::StatusFailed;
    if (parsed) {
      m_speakerStatus = std::move(parsed);
    }
  } else if (!import && !ok) {
    event = E::ApplyFailed;
    m_speakerError = i18n::tr("settings.sound.apply-error");
  } else if (import && !ok) {
    event = E::ImportFailed;
    m_speakerError = i18n::tr(settings::speakerImportErrorKey(err));
  }
  if (const auto after = settings::speakerTransition(m_speakerPhase, event)) {
    m_speakerPhase = *after;
  }
  // Apply and import end in Loading: read the new state.
  if (m_speakerPhase == settings::SpeakerPhase::Loading) {
    startSpeakerCommand({"status"});
  } else if (m_speakerPhase == settings::SpeakerPhase::Ready && m_speakerQueuedApply) {
    auto args = std::move(*m_speakerQueuedApply);
    m_speakerQueuedApply.reset();
    runSpeaker(std::move(args), E::Apply);
  } else {
    m_speakerQueuedApply.reset();
    // Closed during a command: the next open reads the status again.
    if (!isOpen()) {
      if (const auto closed = settings::speakerTransition(m_speakerPhase, E::Close)) {
        m_speakerPhase = *closed;
      }
    }
  }
  if (isOpen()) {
    requestContentRebuild();
  }
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
  std::vector<std::pair<std::string, std::string>> loginScreens;
  for (auto& dm : display_manager::installed()) {
    loginScreens.emplace_back(std::move(dm.unit), std::move(dm.name));
  }
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
          .loginScreens = loginScreens,
          .activeLoginScreen = display_manager::activeUnit(),
          .loginScreenStatus = m_loginScreenStatus,
          .loginScreenError = m_loginScreenError,
          .switchLoginScreen = [this](std::string unit) {
            m_loginScreenStatus = i18n::tr("settings.users.login-screen-switching");
            m_loginScreenError = false;
            requestContentRebuild();
            display_manager::switchTo(m_systemBus, unit, [this, alive = std::weak_ptr<void>(m_aliveGuard)](bool ok, bool denied, std::string error) {
              if (alive.expired()) {
                return;
              }
              m_loginScreenError = !ok;
              m_loginScreenStatus = ok ? i18n::tr("settings.users.login-screen-next-start")
                                       : (denied ? i18n::tr("settings.users.login-screen-denied") : error);
              if (isOpen()) {
                requestContentRebuild();
              }
            });
          },
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
  if (m_selectedSection != "power" || (!m_detail.empty() && !m_detail.starts_with("app-rules:"))) {
    return;
  }
  settings::addSettingsPower(
      *m_contentContainer,
      settings::SettingsPowerContext{
          .scale = scale,
          .upower = m_upower,
          .profiles = m_powerProfilesProvider ? m_powerProfilesProvider() : nullptr,
          .powerConfig = m_powerConfigProvider ? m_powerConfigProvider() : nullptr,
          .health = m_systemHealthProvider ? m_systemHealthProvider() : nullptr,
          .activity = m_activityProvider ? m_activityProvider() : nullptr,
          .activityDetails = m_activityDetails,
          .screenTime = m_screenTime,
          .openActivity = m_openActivityApp,
          .rebuild = [this] { onPowerChanged(); },
          .detail = m_detail,
          .openDetail = [this](std::string detail) { openDetail(std::move(detail)); },
      }
  );
}

void SettingsWindow::onPowerChanged() {
  if (isOpen() && m_selectedSection == "power" && m_searchQuery.empty() && !m_expertOpen) {
    requestContentRebuild();
  }
}

void SettingsWindow::addWorkspacesContent(float scale) {
  if (m_selectedSection != "windows") {
    return;
  }
  settings::addSettingsWorkspaces(
      *m_contentContainer,
      settings::SettingsWorkspacesContext{
          .scale = scale,
          .compositor = m_compositorSettings.get(),
          .detail = m_detail,
      }
  );
}

void SettingsWindow::addAboutContent(float scale) {
  if (m_selectedSection != "about") {
    m_hostnameProxy.reset();
    m_aboutRequested = false;
    return;
  }
  if (!m_detail.empty()) {
    return;
  }
  if (!m_aboutRequested) {
    m_aboutRequested = true;
    SharedWorker::instance().post<settings::AboutHardware>(
        std::weak_ptr<void>(m_aliveGuard), [] { return settings::readAboutHardware(); },
        [this](settings::AboutHardware hardware) {
          m_aboutFacts.hardware = std::move(hardware);
          if (isOpen() && m_selectedSection == "about") {
            requestContentRebuild();
          }
        }
    );
    if (m_systemBus != nullptr) {
      m_hostnameProxy = sdbus::createProxy(
          m_systemBus->connection(), sdbus::ServiceName{kHostnameInterface}, sdbus::ObjectPath{"/org/freedesktop/hostname1"}
      );
      const auto refresh = [this, alive = std::weak_ptr<void>(m_aliveGuard)] {
        m_hostnameProxy->callMethodAsync("GetAll")
            .onInterface(kPropertiesInterface)
            .withArguments(std::string{kHostnameInterface})
            .uponReplyInvoke([this, alive](std::optional<sdbus::Error> err, const VariantMap& properties) {
              if (alive.expired() || err.has_value()) {
                return;
              }
              m_aboutFacts.hostname = stringProperties(properties);
              if (isOpen() && m_selectedSection == "about") {
                requestContentRebuild();
              }
            });
      };
      refresh();
      // Keeps the name current after a rename here or with hostnamectl.
      m_hostnameProxy->uponSignal("PropertiesChanged")
          .onInterface(kPropertiesInterface)
          .call([refresh](const std::string& interfaceName, const VariantMap&, const std::vector<std::string>&) {
            if (interfaceName == kHostnameInterface) {
              refresh();
            }
          });
    }
  }
  const auto rename = [this](std::string name) {
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
    if (m_systemBus == nullptr) {
      finish("no system bus");
      return;
    }
    dbus::callInteractive(
        *m_systemBus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
        "SetPrettyHostname", [&name](sd_bus_message* m) { return sd_bus_message_append(m, "sb", name.c_str(), 1); },
        [bus = m_systemBus, finish, hostname](sd_bus_message* done, std::string error) {
          if (done == nullptr || hostname.empty()) {
            finish(std::move(error));
            return;
          }
          dbus::callInteractive(
              *bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
              "SetStaticHostname",
              [&hostname](sd_bus_message* m) { return sd_bus_message_append(m, "sb", hostname.c_str(), 1); },
              [finish](sd_bus_message*, std::string staticError) { finish(std::move(staticError)); }
          );
        }
    );
  };
  std::string updateStatus;
  if (const SoftwareUpdateService* updates = m_softwareUpdateProvider ? m_softwareUpdateProvider() : nullptr;
      updates != nullptr && updates->available() && !updates->checking()) {
    const std::size_t count = updates->updates().size();
    updateStatus = updates->restartNeeded() ? i18n::tr("settings.about.restart-needed")
        : count == 0                         ? i18n::tr("settings.about.up-to-date")
                                             : i18n::tr("settings.about.updates", "count", std::to_string(count));
  }
  settings::addSettingsAbout(
      *m_contentContainer,
      settings::SettingsAboutContext{
          .scale = scale,
          .facts = &m_aboutFacts,
          .renaming = m_renaming,
          .renameError = m_renameError,
          .updateStatus = std::move(updateStatus),
          .openSoftwareUpdate = [this] { navigateTo("software-update"); },
          .openRename =
              [this, rename](std::string current) {
                if (m_editorSheetModal == nullptr) {
                  m_editorSheetModal = std::make_unique<settings::SettingsSheetModal>();
                  m_editorSheetModal->initialize(m_modalHost, [this] { dismissOpenSelectDropdown(); });
                } else if (m_editorSheetModal->isOpen()) {
                  m_editorSheetModal->close();
                }
                const float sheetScale = uiScale();
                auto draft = std::make_shared<std::string>(current);
                const auto submit = [this, rename, draft, current] {
                  if (!draft->empty() && *draft != current) {
                    rename(*draft);
                  }
                  m_editorSheetModal->close();
                };
                m_editorSheetModal->open(settings::SettingsSheetRequest{
                    .sheetTitle = i18n::tr("settings.about.rename-title"),
                    .populateSheetBody =
                        [this, draft, submit, sheetScale](Flex& body) {
                          body.addChild(ui::input({
                              .value = *draft,
                              .fontSize = Style::fontSizeBody * sheetScale,
                              .controlHeight = Style::controlHeight * sheetScale,
                              .horizontalPadding = Style::spaceSm * sheetScale,
                              .onChange = [draft](const std::string& text) { *draft = text; },
                              .onSubmit = [submit](const std::string&) { submit(); },
                          }));
                          body.addChild(ui::label({
                              .text = i18n::tr("settings.about.rename-hint"),
                              .fontSize = Tokens::typeFootnote.size * sheetScale,
                              .color = colorSpecFromRole(ColorRole::Label2),
                              .maxLines = 2,
                          }));
                          auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End,
                                                  .gap = Style::spaceSm * sheetScale, .fillWidth = true});
                          actions->addChild(settings::page_rows::plainButton(
                              i18n::tr("common.actions.cancel"), ButtonVariant::Default, sheetScale,
                              [this] { m_editorSheetModal->close(); }
                          ));
                          actions->addChild(settings::page_rows::plainButton(
                              i18n::tr("settings.about.rename-action"), ButtonVariant::Primary, sheetScale, submit
                          ));
                          body.addChild(std::move(actions));
                        },
                    .scale = sheetScale,
                    .minWidth = 360.0F * sheetScale,
                    .maxWidth = 420.0F * sheetScale,
                });
              },
          .openLegal = [] { (void)net::openInBrowser("file:///usr/share/licenses"); },
          .copyInfo =
              [this](std::string text) {
                if (m_clipboardService != nullptr) {
                  (void)m_clipboardService->copyText(std::move(text));
                }
              },
      }
  );
}
