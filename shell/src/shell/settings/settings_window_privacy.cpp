#include "auth/biometric_enrollment.h"
#include "auth/password_changer.h"
#include "core/process/process.h"
#include "core/shared_worker.h"
#include "config/config_service.h"
#include "dbus/system_interactive_call.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_alert.h"
#include "shell/settings/settings_content_privacy.h"
#include "shell/settings/settings_window.h"
#include "system/app_permissions.h"

#include <gio/gio.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>

namespace {

  constexpr auto kBiometricsAction = "org.andromeda.biometrics.manage";
  constexpr auto kFaceAdminHelper = "/usr/lib/noctalia/andromeda-face-admin";
  constexpr auto kFingerAdminHelper = "/usr/lib/noctalia/andromeda-finger-admin";

  // nullopt: pam_gaze is not in /etc/pam.d/sudo; true: its line is active (not "#andromeda-off").
  std::optional<bool> faceAdminState() {
    std::ifstream file("/etc/pam.d/sudo");
    for (std::string line; std::getline(file, line);) {
      if (line.find("pam_gaze.so") == std::string::npos || line.find("sufficient") == std::string::npos) {
        continue;
      }
      return !line.starts_with("#");
    }
    return std::nullopt;
  }

  // An active pam_fprintd line in sudo, or in the stack every login includes (Debian's pam-auth-update).
  bool fingerAdminState() {
    for (const char* path : {"/etc/pam.d/sudo", "/etc/pam.d/common-auth", "/etc/pam.d/system-auth"}) {
      std::ifstream file(path);
      for (std::string line; std::getline(file, line);) {
        if (line.starts_with("auth") && line.find("pam_fprintd.so") != std::string::npos) {
          return true;
        }
      }
    }
    return false;
  }

  std::string currentUser() {
    const passwd* pw = getpwuid(getuid());
    return pw != nullptr && pw->pw_name != nullptr ? pw->pw_name : "";
  }

  // Field 22 of /proc/self/stat: polkit pairs it with the pid so a reused pid is not trusted.
  std::uint64_t processStartTime() {
    std::ifstream file("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto close = stat.rfind(')');
    if (close == std::string::npos) {
      return 0;
    }
    std::istringstream fields(stat.substr(close + 2));
    std::string field;
    // After ")" the fields start at 3 (state); start-time is field 22.
    for (int i = 3; i <= 22 && (fields >> field); ++i) {
    }
    try {
      return std::stoull(field);
    } catch (...) {
      return 0;
    }
  }

} // namespace

void SettingsWindow::authorizeBiometrics(std::function<void()> then) {
  GVariantBuilder subject;
  g_variant_builder_init(&subject, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&subject, "{sv}", "pid", g_variant_new_uint32(static_cast<std::uint32_t>(getpid())));
  g_variant_builder_add(&subject, "{sv}", "start-time", g_variant_new_uint64(processStartTime()));
  GVariantBuilder details;
  g_variant_builder_init(&details, G_VARIANT_TYPE("a{ss}"));
  constexpr std::uint32_t kAllowUserInteraction = 1;
  dbus::callInteractive(
      "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority", "org.freedesktop.PolicyKit1.Authority",
      "CheckAuthorization",
      g_variant_new("((sa{sv})sa{ss}us)", "unix-process", &subject, kBiometricsAction, &details,
                    kAllowUserInteraction, ""),
      [this, alive = std::weak_ptr<void>(m_aliveGuard), then = std::move(then)](GVariant* result, std::string error) {
        if (alive.expired()) {
          return;
        }
        gboolean authorized = FALSE;
        if (result != nullptr) {
          g_variant_get(result, "((bb@a{ss}))", &authorized, nullptr, nullptr);
        }
        if (authorized == FALSE) {
          m_privacyStatus = error.empty() ? i18n::tr("settings.privacy.not-allowed") : error;
          if (isOpen()) {
            requestContentRebuild();
          }
          return;
        }
        then();
      }
  );
}

// passwd runs on SharedWorker: a wrong password holds it for the pam_faillock delay (about 2 s), a stalled PAM
// stack up to a minute (password_changer.h).
void SettingsWindow::changePassword() {
  settings::PasswordDraft& draft = m_passwordDraft;
  if (draft.busy) {
    return;
  }
  const auto refuse = [this, &draft](const char* key) {
    draft.status = i18n::tr(key);
    draft.statusError = true;
    requestContentRebuild();
  };
  if (draft.current.empty() || draft.next.empty()) {
    refuse("settings.privacy.password-empty");
    return;
  }
  if (draft.next != draft.verify) {
    refuse("settings.privacy.password-mismatch");
    return;
  }
  draft.busy = true;
  draft.status.clear();
  requestContentRebuild();
  SharedWorker::instance().post<password_change::Result>(
      m_aliveGuard,
      [current = draft.current, next = draft.next]() mutable { return password_change::change(current, next); },
      [this](password_change::Result result) {
        settings::PasswordDraft& d = m_passwordDraft;
        d.busy = false;
        d.statusError = !result.ok;
        if (result.ok) {
          d.status = i18n::tr(result.keyringFailed ? "settings.privacy.password-changed-keyring"
                                                   : "settings.privacy.password-changed");
        } else if (result.wrongCurrent) {
          d.status = i18n::tr("settings.privacy.password-wrong");
        } else {
          d.status = result.message.empty() ? i18n::tr("settings.privacy.change-failed") : result.message;
        }
        for (std::string* field : {&d.current, &d.next, &d.verify}) {
          std::fill(field->begin(), field->end(), '\0');
          field->clear();
        }
        if (isOpen()) {
          requestContentRebuild();
        }
      }
  );
}

void SettingsWindow::loadAppAccess() {
  static constexpr std::pair<const char*, const char*> kMeta[] = {
      {"settings.privacy.camera", "camera"},           {"settings.privacy.location", "map-pin"},
      {"settings.privacy.screenshots", "screenshot"},  {"settings.privacy.screen-sharing", "screen-share"},
      {"settings.privacy.remote-control", "device-desktop-cog"},
  };
  const auto& kinds = AppPermissions::kinds();
  if (m_access.empty()) {
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      m_access.push_back({.table = kinds[i].table, .titleKey = kMeta[i].first, .glyph = kMeta[i].second,
                          .tokens = kinds[i].id.empty()});
    }
  }
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    m_permissions->load(kinds[i], [this, i](AppPermissions::Grants grants) {
      auto& view = m_access[i];
      view.loaded = true;
      view.apps.clear();
      for (const auto& grant : grants) {
        view.apps.push_back({.id = grant.app,
                             .label = grant.app.empty() ? i18n::tr("settings.privacy.host-apps") : grant.app,
                             .allowed = grant.allowed});
      }
      if (isOpen() && m_selectedSection == "privacy") {
        requestContentRebuild();
      }
    });
  }
}

void SettingsWindow::leavePrivacy() {
  if (m_enroll != nullptr && m_enroll->enrolling() != BiometricEnrollment::Method::None) {
    m_enroll->stop();
  }
  if (!m_passwordDraft.busy) {
    for (std::string* field : {&m_passwordDraft.current, &m_passwordDraft.next, &m_passwordDraft.verify}) {
      std::fill(field->begin(), field->end(), '\0');
      field->clear();
    }
    m_passwordDraft.status.clear();
  }
}

void SettingsWindow::addPrivacyContent(float scale) {
  if (m_selectedSection != "privacy") {
    leavePrivacy();
    return;
  }
  if (m_permissions == nullptr && m_sessionBus != nullptr) {
    m_permissions = std::make_unique<AppPermissions>(*m_sessionBus);
    loadAppAccess();
  }
  if (m_enroll == nullptr && m_systemBus != nullptr) {
    m_enroll = std::make_unique<BiometricEnrollment>(m_systemBus, currentUser());
    m_enroll->setChangedCallback([this] {
      if (isOpen() && m_selectedSection == "privacy") {
        requestContentRebuild();
      }
    });
    m_enroll->setFinishedCallback([this](BiometricEnrollment::Method /*method*/, bool ok) {
      m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.enroll-failed");
    });
    m_enroll->probe();
  }
  const auto& lock = m_config->config().lockscreen;
  // A 250-byte root-readable file in /etc: no disk wait worth a worker.
  const std::optional<bool> faceAdmin = faceAdminState();
  BiometricEnrollment* enroll = m_enroll.get();
  const auto done = [this](bool ok) {
    m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.change-failed");
    if (isOpen()) {
      requestContentRebuild();
    }
  };
  const auto confirmRemove = [this, scale](std::string name, std::function<void()> remove) {
    settings::showSettingsAlert(
        m_modalHost,
        settings::SettingsAlertRequest{
            .title = i18n::tr("settings.privacy.remove-confirm", "name", name),
            .buttons = {
                {.text = i18n::tr("common.actions.cancel")},
                {.text = i18n::tr("settings.privacy.remove"),
                 .variant = ButtonVariant::Destructive,
                 .onClick = std::move(remove)},
            },
            // Cancel is the default for a destructive action, so Enter never removes.
            .defaultIndex = 0,
            .scale = scale,
        }
    );
  };
  const auto enrolling = enroll == nullptr ? BiometricEnrollment::Method::None : enroll->enrolling();
  settings::addSettingsPrivacy(
      *m_contentContainer,
      settings::SettingsPrivacyContext{
          .scale = scale,
          .detail = m_detail,
          .biometricsKnown = enroll != nullptr,
          .facePresent = enroll != nullptr && enroll->facePresent(),
          .fingerPresent = enroll != nullptr && enroll->fingerPresent(),
          .fprintdMissing = enroll != nullptr && enroll->fprintdMissing(),
          .faces = enroll != nullptr ? enroll->faces() : std::vector<std::string>{},
          .fingers = enroll != nullptr ? enroll->fingers() : std::vector<std::string>{},
          .enrolling = enrolling == BiometricEnrollment::Method::Face ? 1
                       : enrolling == BiometricEnrollment::Method::Finger ? 2
                                                                          : 0,
          .enrollProgress = enroll != nullptr ? enroll->progress() : 0.0F,
          .enrollPrompt = enroll != nullptr ? enroll->prompt() : std::string{},
          .status = m_privacyStatus,
          .faceUnlock = lock.face,
          .faceOnRequest = lock.faceOnRequest,
          .fingerprintUnlock = lock.fingerprint,
          .faceAdminKnown = faceAdmin.has_value() && ::access(kFaceAdminHelper, X_OK) == 0,
          .faceAdmin = faceAdmin.value_or(false),
          .setFaceAdmin =
              [this](bool on) {
                const bool started = process::runAsync(
                    std::vector<std::string>{"pkexec", kFaceAdminHelper, on ? "on" : "off"},
                    process::RunCallbacks{
                        .onExit =
                            [this, alive = std::weak_ptr<void>(m_aliveGuard)](const process::RunResult& result) {
                              if (alive.expired()) {
                                return;
                              }
                              m_privacyStatus = result ? std::string{} : i18n::tr("settings.privacy.change-failed");
                              if (isOpen()) {
                                requestContentRebuild();
                              }
                            },
                    }
                );
                if (!started) {
                  m_privacyStatus = i18n::tr("settings.privacy.change-failed");
                  requestContentRebuild();
                }
              },
          .fingerAdminKnown = ::access(kFingerAdminHelper, X_OK) == 0,
          .fingerAdmin = fingerAdminState(),
          .setFingerAdmin =
              [this](bool on) {
                const bool started = process::runAsync(
                    std::vector<std::string>{"pkexec", kFingerAdminHelper, on ? "on" : "off"},
                    process::RunCallbacks{
                        .onExit =
                            [this, alive = std::weak_ptr<void>(m_aliveGuard)](const process::RunResult& result) {
                              if (alive.expired()) {
                                return;
                              }
                              m_privacyStatus = result ? std::string{} : i18n::tr("settings.privacy.change-failed");
                              if (isOpen()) {
                                requestContentRebuild();
                              }
                            },
                    }
                );
                if (!started) {
                  m_privacyStatus = i18n::tr("settings.privacy.change-failed");
                  requestContentRebuild();
                }
              },
          .openDetail =
              [this](std::string detail) {
                m_privacyStatus.clear();
                openDetail(std::move(detail));
              },
          .back =
              [this] {
                m_privacyStatus.clear();
                leavePrivacy();
                leaveDetail();
              },
          .setBool = [this](std::vector<std::string> path, bool value) { setSettingOverride(std::move(path), value); },
          // gazed asks polkit itself (com.gundulabs.gaze.manage-faces) before it stores a face.
          .addFace =
              [this, enroll] {
                m_privacyStatus.clear();
                enroll->start(BiometricEnrollment::Method::Face);
              },
          .addFinger =
              [this, enroll] {
                m_privacyStatus.clear();
                authorizeBiometrics([enroll] { enroll->start(BiometricEnrollment::Method::Finger); });
              },
          .cancelEnroll = [this, enroll] {
            enroll->stop();
            requestContentRebuild();
          },
          .removeFace =
              [enroll, done, confirmRemove](std::string name) {
                confirmRemove(name, [enroll, done, name] { enroll->removeFace(name, done); });
              },
          .renameFace =
              [this, enroll, done](std::string from, std::string to) {
                enroll->renameFace(from, to, [this, done](bool ok) {
                  done(ok);
                  if (ok && m_detail.ends_with("\trename")) {
                    leaveDetail();
                  }
                });
              },
          .removeFinger =
              [this, enroll, done, confirmRemove](std::string finger) {
                confirmRemove(BiometricEnrollment::fingerLabel(finger), [this, enroll, done, finger] {
                  authorizeBiometrics([enroll, done, finger] { enroll->removeFinger(finger, done); });
                });
              },
          .password = &m_passwordDraft,
          .access = m_access,
          .setAccess =
              [this](std::string table, std::string app, bool allowed) {
                const auto& kinds = AppPermissions::kinds();
                const auto kind = std::ranges::find(kinds, table, &AppPermissions::Kind::table);
                if (kind == kinds.end() || m_permissions == nullptr) {
                  return;
                }
                // The grant's store entries come from a fresh load: the view keeps only what the rows show.
                m_permissions->load(*kind, [this, kind = *kind, app, allowed](AppPermissions::Grants grants) {
                  const auto grant = std::ranges::find(grants, app, &AppPermissions::Grant::app);
                  if (grant == grants.end()) {
                    loadAppAccess();
                    return;
                  }
                  m_permissions->setAllowed(kind, *grant, allowed, [this](bool ok) {
                    m_privacyStatus = ok ? std::string{} : i18n::tr("settings.privacy.change-failed");
                    loadAppAccess();
                  });
                });
              },
          .changePassword = [this] { changePassword(); },
      }
  );
}
