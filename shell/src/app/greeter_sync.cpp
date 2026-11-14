#include "app/greeter_sync.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "theme/theme_service.h"
#include "util/string_utils.h"

#include <chrono>
#include <utility>

GreeterSync::GreeterSync(
    ConfigService& config, noctalia::theme::ThemeService& theme, CompositorPlatform& platform, Hooks hooks
)
    : m_config(config), m_theme(theme), m_platform(platform), m_hooks(std::move(hooks)) {}

GreeterSync::~GreeterSync() { greeter::setLockWallpaperBakeReady({}); }

void GreeterSync::start() {
  m_lockscreen = m_config.config().lockscreen;
  greeter::setLockWallpaperBakeReady([this] { scheduleAuto(); });
}

void GreeterSync::onConfigReload() {
  if (m_config.config().lockscreen != m_lockscreen) {
    m_lockscreen = m_config.config().lockscreen;
    scheduleAuto();
  }
}

void GreeterSync::reportFailure(bool quiet) {
  if (quiet) {
    notify::error("Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter"));
  } else {
    m_hooks.settingsError(i18n::tr("settings.errors.sync-greeter"));
  }
}

void GreeterSync::sync(bool quiet) {
  if (!greeter::appearanceSyncAvailable(m_config.config().shell.greeterSync)) {
    return;
  }

  const std::uint64_t generation = m_generation + 1;

  const auto complete = [this, generation, quiet](bool success) {
    DeferredCall::callLater([this, generation, quiet, success] {
      if (generation != m_generation) {
        return;
      }
      m_timeoutTimer.stop();
      if (success) {
        if (!quiet) {
          notify::info(
              "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
              i18n::tr("notifications.internal.greeter-sync-success")
          );
        }
        return;
      }
      reportFailure(quiet);
    });
  };

  const auto launch = greeter::syncAppearanceToGreeterAsync(
      m_config, m_theme.resolvedShellMode(), complete, &m_platform, m_hooks.logindOnSystemBus()
  );
  if (launch == greeter::GreeterSyncLaunch::Busy) {
    // A change (or a finished wallpaper bake) during a running sync syncs again after it, or the greeter keeps the
    // older look until an unrelated sync.
    if (quiet) {
      scheduleAuto();
    }
    return;
  }

  m_generation = generation;
  m_timeoutTimer.stop();
  if (launch == greeter::GreeterSyncLaunch::Failed) {
    reportFailure(quiet);
    return;
  }
  if (launch == greeter::GreeterSyncLaunch::StagedOnly) {
    if (quiet) {
      reportFailure(quiet);
    } else {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
          i18n::tr("notifications.internal.greeter-sync-pending-manual")
      );
    }
    return;
  }
  if (quiet) {
    return;
  }
  const bool legacySync = launch == greeter::GreeterSyncLaunch::LaunchedLegacy;
  const bool customPrivilege = !StringUtils::trim(m_config.config().shell.greeterSync.privilegeCommand).empty();
  const bool polkitAgentActive = m_hooks.polkitAgentActive();
  const bool inSessionPolkit = m_hooks.inSessionPolkit();
  const char* pendingBodyKey =
      legacySync ? "notifications.internal.greeter-sync-pending-legacy" : "notifications.internal.greeter-sync-pending";
  if (!legacySync && !customPrivilege && !polkitAgentActive && !inSessionPolkit) {
    pendingBodyKey = "notifications.internal.greeter-sync-pending-manual-pkexec";
  } else if (!legacySync && !customPrivilege && !polkitAgentActive) {
    pendingBodyKey = "notifications.internal.greeter-sync-pending-console";
  }
  notify::info("Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr(pendingBodyKey));

  m_timeoutTimer.start(std::chrono::seconds(90), [this, generation, inSessionPolkit, legacySync] {
    if (generation != m_generation) {
      return;
    }
    DeferredCall::callLater([this, generation, inSessionPolkit, legacySync] {
      if (generation != m_generation) {
        return;
      }
      notify::error(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
          i18n::tr(
              inSessionPolkit  ? "notifications.internal.greeter-sync-timeout"
                  : legacySync ? "notifications.internal.greeter-sync-timeout-manual"
                               : "notifications.internal.greeter-sync-timeout-manual-pkexec"
          )
      );
      m_hooks.settingsError(
          i18n::tr(
              inSessionPolkit  ? "settings.errors.sync-greeter-timeout"
                  : legacySync ? "settings.errors.sync-greeter-timeout-manual"
                               : "settings.errors.sync-greeter-timeout-manual-pkexec"
          )
      );
    });
  });
}

void GreeterSync::scheduleAuto() {
  if (!m_config.config().shell.greeterSync.autoSync
      || !greeter::appearanceSyncAvailable(m_config.config().shell.greeterSync)) {
    return;
  }
  m_autoTimer.stop();
  m_autoTimer.start(std::chrono::milliseconds(1000), [this] { sync(true); });
}
