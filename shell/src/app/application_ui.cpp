#include "app/main_loop.h"
#include "application.h"
#include "application_internal.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "system/crash_report.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/link/link_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/notification/kde_notification_client.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_service.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/polkit/polkit_session_support.h"
#include "dbus/portal/settings_portal.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/udisks/udisks_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "launcher/app_provider.h"
#include "launcher/dmenu_provider.h"
#include "launcher/clipboard_provider.h"
#include "launcher/emoji_provider.h"
#include "launcher/math_provider.h"
#include "launcher/panel_provider.h"
#include "launcher/plugin_launcher_provider.h"
#include "launcher/session_provider.h"
#include "launcher/files_provider.h"
#include "launcher/klipy_provider.h"
#include "launcher/symbol_provider.h"
#include "launcher/web_search_provider.h"
#include "launcher/wallpaper_provider.h"
#include "launcher/window_provider.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/clipboard/clipboard_paste.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/display_mode/display_mode_panel.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/plugin_panel.h"
#include "shell/phone/phone_panel.h"
#include "shell/polkit/polkit_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/setup_wizard/setup_wizard_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tray/tray_drawer_panel.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/app_identity.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/distro_info.h"
#include "system/easyeffects_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/input.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "theme/toolkit_theme_files.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <malloc.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

  // App ids of the windows open before the last compositor crash (written by the compositor, docs/resilience.md §5).
  std::vector<std::string> crashSessionAppIds() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path path = state != nullptr && *state != '\0'
        ? std::filesystem::path(state) / "andromeda/session-crash.json"
        : std::filesystem::path(home != nullptr ? home : "") / ".local/state/andromeda/session-crash.json";
    std::ifstream in(path);
    const nlohmann::json session = nlohmann::json::parse(in, nullptr, false);
    std::vector<std::string> ids;
    if (session.is_discarded() || !session.contains("windows") || !session["windows"].is_array()) {
      return ids;
    }
    for (const auto& window : session["windows"]) {
      const std::string id = window.value("app_id", "");
      if (!id.empty() && std::ranges::find(ids, id) == ids.end()) {
        ids.push_back(id);
      }
    }
    return ids;
  }

  constexpr Logger kLog("app");

  [[nodiscard]] bool overviewTypeToLaunchEnabled(const ConfigService& configService) {
    if (compositors::isAndromeda()) {
      return configService.config().shell.andromedaOverviewTypeToLaunchEnabled;
    }

    if (compositors::isNiri()) {
      return configService.config().shell.niriOverviewTypeToLaunchEnabled;
    }
    return false;
  }

  [[nodiscard]] bool overviewCloseOnLaunchEnabled(const ConfigService& configService) {
    if (compositors::isAndromeda()) {
      return configService.config().shell.andromedaOverviewCloseOnLaunchEnabled;
    }

    if (compositors::isNiri()) {
      return configService.config().shell.niriOverviewCloseOnLaunchEnabled;
    }
    return false;
  }
} // namespace

void Application::initUi() {
  initUiRenderSurfacesAndSettings();
  initLockScreenAndSession();
  initInputDispatch();
  initPanelManagerAndPanels();
  initNotificationAndOsd();
  initBarDockAndLayout();
  initWidgetControllersAndCallbacks();
  // Wiring is complete and outputs are enumerated; build every per-output layer
  // surface once in canonical order. initialize() above only wired dependencies.
  reconcileOutputSurfaces();
}

void Application::initUiRenderSurfacesAndSettings() {
  m_glSharedInit.get();
  m_renderContext.initialize(m_glShared);
  m_glShared.setGraphicsResetCallback([this](RenderGraphicsResetStatus status) { onGraphicsReset(status); });
  if (!m_glShared.hasSharedContext()) {
    m_asyncTextureCache.setMakeCurrentCallback([this] { m_renderContext.backend().makeCurrentNoSurface(); });
  }
  m_renderContext.setTextFontFamily(m_configService.config().shell.fontFamily);
  Style::setRtl(i18n::Service::instance().rtl());
  m_renderContext.setTextBaseDirection(i18n::Service::instance().rtl());
  m_wallpaper.initialize(m_wayland, &m_configService, &m_renderContext, &m_sharedTextureCache, &m_themeService);
  m_backdrop.initialize(m_wayland, &m_configService, &m_sharedTextureCache, &m_glShared);
  m_settingsWindow.initialize(
      m_wayland, &m_configService, &m_renderContext, &m_dependencyService, upower(), &m_idleManager,
      &m_compositorPlatform, m_accountsService.get()
  );
  m_settingsWindow.setSystemBus(m_systemBus.get());
  m_settingsWindow.setConnectivityProviders([this] { return network(); }, [this] { return bluetooth(); });
  m_settingsWindow.setAudioProvider([this] { return pipewire(); });
  m_settingsWindow.setPowerProfilesProvider([this] { return powerProfiles(); });
  m_settingsWindow.setPowerConfigProvider([this] { return m_powerConfigService.get(); });
  m_settingsWindow.setFileWatcher(&m_fileWatcher);
  m_settingsWindow.setPluginManager(&m_pluginManager);
  m_settingsWindow.setPluginRecommendations(&m_pluginRecommendations);
  m_settingsWindow.setIpcService(&m_ipcService);
  m_settingsWindow.setActivity(
      [this]() -> std::optional<std::size_t> {
        return m_backgroundActivityMonitor != nullptr ? m_backgroundActivityMonitor->flaggedCount() : std::nullopt;
      },
      [this] { m_activityWindow.open({}); }
  );
  m_settingsWindow.setAsyncTextureCache(&m_asyncTextureCache);
  m_settingsWindow.setOpenDesktopWidgetEditor([this] {
    if (m_lockscreenWidgetsController.isEditing()) {
      m_lockscreenWidgetsController.exitEdit();
    }
    const bool wasEditing = m_desktopWidgetsController.isEditing();
    m_desktopWidgetsController.toggleEdit();
    if (!wasEditing && m_desktopWidgetsController.isEditing()) {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.desktop-widgets-editor"),
          i18n::tr("notifications.internal.desktop-widgets-editor-enabled")
      );
    }
  });
  m_settingsWindow.setOpenLockscreenWidgetEditor([this] {
    if (!m_configService.isLockScreenEnabled()) {
      return;
    }
    if (m_lockScreen.isActive()) {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.lockscreen-widgets-editor"),
          i18n::tr("notifications.internal.lockscreen-widgets-editor-blocked-locked")
      );
      return;
    }
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.exitEdit();
    }
    m_lockscreenWidgetsController.toggleEdit();
  });
  m_settingsWindow.setSyncGreeterAppearance([this] { performGreeterSync(); });
  m_settingsWindow.setSaveWallpaperPaletteAsCustom([this] {
    std::string paletteName;
    std::string error;
    if (!m_themeService.saveWallpaperPaletteAsCustom(&paletteName, &error)) {
      m_settingsWindow.markSettingsWriteError(
          error.empty() ? i18n::tr("settings.errors.export-wallpaper-palette") : std::move(error)
      );
      return;
    }
    m_settingsWindow.onExternalOptionsChanged();
    m_settingsWindow.markSettingsWriteSuccess(true);
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.wallpaper-palette-export"),
        i18n::tr("notifications.internal.wallpaper-palette-export-success", "name", paletteName)
    );
  });
}

void Application::performGreeterSync(bool quiet) {
  if (!greeter::appearanceSyncAvailable(m_configService.config().shell.greeterSync)) {
    return;
  }

  const std::uint64_t generation = m_greeterSyncGeneration + 1;

  const auto complete = [this, generation, quiet](bool success) {
    DeferredCall::callLater([this, generation, quiet, success] {
      if (generation != m_greeterSyncGeneration) {
        return;
      }
      m_greeterSyncTimeoutTimer.stop();
      if (success) {
        if (!quiet) {
          notify::info(
              "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
              i18n::tr("notifications.internal.greeter-sync-success")
          );
        }
        return;
      }
      if (quiet) {
        notify::error(
            "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
        );
      } else {
        m_settingsWindow.markSettingsWriteError(i18n::tr("settings.errors.sync-greeter"));
      }
    });
  };

  const auto launch = greeter::syncAppearanceToGreeterAsync(
      m_configService, m_themeService.resolvedShellMode(), complete, &m_compositorPlatform, m_logindService != nullptr
  );
  if (launch == greeter::GreeterSyncLaunch::Busy) {
    return;
  }

  m_greeterSyncGeneration = generation;
  m_greeterSyncTimeoutTimer.stop();
  if (launch == greeter::GreeterSyncLaunch::Failed) {
    if (quiet) {
      notify::error(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
      );
    } else {
      m_settingsWindow.markSettingsWriteError(i18n::tr("settings.errors.sync-greeter"));
    }
    return;
  }
  if (launch == greeter::GreeterSyncLaunch::StagedOnly) {
    if (quiet) {
      notify::error(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr("settings.errors.sync-greeter")
      );
    } else {
      notify::info(
          "Noctalia", i18n::tr("notifications.internal.greeter-sync"),
          i18n::tr("notifications.internal.greeter-sync-pending-manual")
      );
    }
    return;
  }
  const bool legacySync = launch == greeter::GreeterSyncLaunch::LaunchedLegacy;
  if (!quiet) {
    const bool customPrivilege =
        !StringUtils::trim(m_configService.config().shell.greeterSync.privilegeCommand).empty();
    const bool polkitAgentActive = m_configService.config().shell.polkitAgent && m_polkitAgent != nullptr;
    const bool inSessionPolkit = likelySupportsInSessionPolkit();
    const char* pendingBodyKey = legacySync ? "notifications.internal.greeter-sync-pending-legacy"
                                            : "notifications.internal.greeter-sync-pending";
    if (!legacySync && !customPrivilege && !polkitAgentActive && !inSessionPolkit) {
      pendingBodyKey = "notifications.internal.greeter-sync-pending-manual-pkexec";
    } else if (!legacySync && !customPrivilege && !polkitAgentActive) {
      pendingBodyKey = "notifications.internal.greeter-sync-pending-console";
    }
    notify::info("Noctalia", i18n::tr("notifications.internal.greeter-sync"), i18n::tr(pendingBodyKey));
  }

  if (!quiet) {
    const bool inSessionPolkit = likelySupportsInSessionPolkit();
    m_greeterSyncTimeoutTimer.start(std::chrono::seconds(90), [this, generation, inSessionPolkit, legacySync] {
      if (generation != m_greeterSyncGeneration) {
        return;
      }
      DeferredCall::callLater([this, generation, inSessionPolkit, legacySync] {
        if (generation != m_greeterSyncGeneration) {
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
        m_settingsWindow.markSettingsWriteError(
            i18n::tr(
                inSessionPolkit  ? "settings.errors.sync-greeter-timeout"
                    : legacySync ? "settings.errors.sync-greeter-timeout-manual"
                                 : "settings.errors.sync-greeter-timeout-manual-pkexec"
            )
        );
      });
    });
  }
}

void Application::scheduleGreeterAutoSync() {
  if (!m_configService.config().shell.greeterSync.autoSync
      || !greeter::appearanceSyncAvailable(m_configService.config().shell.greeterSync)) {
    return;
  }
  m_greeterAutoSyncTimer.stop();
  m_greeterAutoSyncTimer.start(std::chrono::milliseconds(1000), [this] { performGreeterSync(true); });
}

void Application::initLockScreenAndSession() {
  m_lockScreen.initialize(
      m_wayland, &m_renderContext, &m_configService, &m_sharedTextureCache, m_systemBus.get(), &m_compositorPlatform
  );
  m_lockScreen.setLoginBoxServices(&m_sessionActionRunner, mpris(), &m_weatherService, &m_httpClient);
  m_lockScreen.setNotificationManager(&m_notificationManager);
  refreshLockStatus();
  m_wallpaper.setAutomationGate([this] {
    if (m_lockScreen.isActive()) {
      return false;
    }
    return !m_configService.config().wallpaper.automation.pauseOnFullscreen || !m_wayland.hasFullscreenToplevel();
  });
  m_configService.addReloadCallback([this] {
    if (m_logindService != nullptr) {
      m_logindService->setSessionLockIntegrationEnabled(m_configService.isLockScreenEnabled());
      m_logindService->setLockBeforeSuspendEnabled(m_configService.shouldLockBeforeSuspend());
    }
    m_lockScreen.onConfigChanged();
    m_lockscreenWidgetsController.onLockStateChanged();
  });
  m_lockScreen.setSessionHooks(
      [this] {
        m_idleGraceOverlay.hide();
        m_lockscreenWidgetsController.onLockStateChanged();
        m_idleManager.setSessionLocked(true);
        m_screenTimeService.setSessionLocked(true);
        m_hookManager.fire(HookKind::SessionLocked);
        if (m_logindService != nullptr) {
          m_logindService->setSessionLockedHint(true);
        }
        if (screenSaver() != nullptr) {
          screenSaver()->emitActiveChanged(true);
        }
      },
      [this] {
        m_idleGraceOverlay.hide();
        m_lockscreenWidgetsController.onLockStateChanged();
        m_idleManager.setSessionLocked(false);
        m_screenTimeService.setSessionLocked(false);
        m_hookManager.fire(HookKind::SessionUnlocked);
        releaseSleepDelayInhibitIfPending();
        requestAllSurfacesRedraw();
        // The bar skipped ticks while locked; refresh it now rather than at the next minute.
        DeferredCall::callLater([this] { m_timeService.forceTick(); });
        if (m_logindService != nullptr) {
          m_logindService->setSessionLockedHint(false);
        }
        if (screenSaver() != nullptr) {
          screenSaver()->emitActiveChanged(false);
        }
      },
      [this] {
        m_idleGraceOverlay.hide();
        m_lockscreenWidgetsController.onLockStateChanged();
        syncLockStreamPause();
        releaseSleepDelayInhibitIfPending();
        requestAllSurfacesRedraw();
      },
      [this] { releaseSleepDelayInhibitIfPending(); }
  );
  m_compositorPlatform.setOutputPowerChangeCallback([this] {
    m_lockScreen.onOutputPowerChanged();
    syncLockStreamPause();
  });
  if (m_logindService != nullptr) {
    m_logindService->setSessionLockIntegrationEnabled(m_configService.isLockScreenEnabled());
    m_logindService->setLockBeforeSuspendEnabled(m_configService.shouldLockBeforeSuspend());
    m_logindService->setLockCallback([this] {
      if (!m_configService.isLockScreenEnabled()) {
        return;
      }
      if (!m_lockScreen.isActive()) {
        (void)m_lockScreen.lock();
      }
    });
    m_logindService->setUnlockCallback([this] {
      if (m_lockScreen.isActive()) {
        m_lockScreen.unlock();
      }
    });
  }

  SessionActionHooks sessionActionHooks;
  sessionActionHooks.onLogout = [this](std::function<void(bool)> done) {
    m_hookManager.fireAsync(HookKind::LoggingOut, std::move(done));
  };
  sessionActionHooks.onReboot = [this](std::function<void(bool)> done) {
    m_hookManager.fireAsync(HookKind::Rebooting, std::move(done));
  };
  sessionActionHooks.onShutdown = [this](std::function<void(bool)> done) {
    m_hookManager.fireAsync(HookKind::ShuttingDown, std::move(done));
  };
  sessionActionHooks.onBeforePlainSuspend = [this] { m_skipLockOnNextSleep = true; };
  sessionActionHooks.onPlainSuspendAborted = [this] { m_skipLockOnNextSleep = false; };
  m_sessionActionRunner.setHooks(std::move(sessionActionHooks));
  m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power);
  m_configService.addReloadCallback(
      [this] { m_sessionActionRunner.setPowerConfig(m_configService.config().shell.session.power); }, "session-power"
  );
}

void Application::initInputDispatch() {
  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onPointerEvent(event);
      return;
    }
    if (m_windowSwitcher.isActive()) {
      if (m_windowSwitcher.onPointerEvent(event)) {
        return;
      }
      if (event.type == PointerEvent::Type::Button || event.type == PointerEvent::Type::Axis) {
        return;
      }
      // Enter/Leave/Motion fall through so other surfaces' hover state stays in sync.
    }
    if (m_colorPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_glyphPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_fileDialogPopup.onPointerEvent(event)) {
      return;
    }
    if (m_searchPickerDialogPopup.onPointerEvent(event)) {
      return;
    }
    // Region overlay is layer Overlay + exclusive keyboard; prefer it over the
    // widgets editors (Bottom / OnDemand) so confirm/cancel still work mid-edit.
    if (m_screenshotService.onPointerEvent(event)) {
      return;
    }
    if (m_pluginLayerHost.onPointerEvent(event)) {
      return;
    }
    if (m_lockscreenWidgetsController.onPointerEvent(event)) {
      return;
    }
    if (m_desktopWidgetsController.onPointerEvent(event)) {
      return;
    }
    if (m_wallpaper.onPointerEvent(event)) {
      return;
    }
    if (m_trayMenu.onPointerEvent(event)) {
      return;
    }
    if (m_settingsWindow.onPointerEvent(event))
      return;
    if (m_activityWindow.onPointerEvent(event))
      return;
    if (m_liveActivity.onPointerEvent(event))
      return;
    if (m_bar.onPointerEvent(event))
      return;
    if (m_dock.onPointerEvent(event))
      return;
    if (m_panelManager.onPointerEvent(event))
      return;
    if (m_hotCorners.onPointerEvent(event))
      return;
    m_notificationToast.onPointerEvent(event);
  });

  // The lock client died, or the compositor restarted into the lock (docs/resilience.md §4): lock again at once.
  m_wayland.setRelockCallback([this] {
    if (!m_lockScreen.isActive()) {
      m_lockScreen.lock();
    }
  });
  // One notification per restart; "restore" relaunches every saved app that has no window when it is clicked, so
  // apps that reconnected meanwhile (Qt) are not started twice.
  auto restoreId = std::make_shared<std::uint32_t>(0);
  m_notificationManager.addInternalActionCallback(
      [this, restoreId](std::uint32_t id, const std::string& action, const std::string& token, const std::string&) {
        if (id != *restoreId) {
          return;
        }
        if (action == "report") {
          crash_report::save(0);
          return;
        }
        if (action != "restore") {
          return;
        }
        const std::vector<std::string> running = m_wayland.runningAppIds();
        for (const std::string& appId : crashSessionAppIds()) {
          if (std::ranges::find(running, appId) != running.end()) {
            continue;
          }
          if (const auto entry = app_identity::findDesktopEntry(appId, desktopEntries()); entry.has_value()) {
            (void)desktop_entry_launch::launchEntry(*entry, {.activationToken = token});
          }
        }
      }
  );
  m_wayland.setRestartedCallback([this, restoreId] {
    const std::vector<std::string> saved = crashSessionAppIds();
    kLog.info("the compositor restarted after a crash; {} apps were open", saved.size());
    NotificationRequest request;
    request.appName = i18n::tr("notifications.internal.desktop");
    request.summary = i18n::tr("notifications.internal.desktop-restarted");
    request.body = saved.empty() ? i18n::tr("notifications.internal.desktop-restarted-none")
                                 : i18n::tr("notifications.internal.desktop-restarted-body", "count",
                                            std::to_string(saved.size()));
    request.origin = NotificationOrigin::Internal;
    request.timeout = 0; // stays until dismissed: the user may come back to it after the restart
    request.persistInHistory = true;
    if (!saved.empty()) {
      request.actions = {"restore", i18n::tr("notifications.internal.desktop-restore")};
    }
    request.actions.push_back("report");
    request.actions.push_back(i18n::tr("notifications.internal.desktop-report"));
    *restoreId = m_notificationManager.addOrReplace(std::move(request));
  });
  m_wayland.setLockKeysChangeCallback([this] {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onLockKeysChanged();
    }
  });
  m_wayland.setKeyboardModifiersCallback([this](std::uint32_t modifiers) {
    m_windowSwitcher.onKeyboardModifiers(modifiers);
    m_screenshotService.onModifiersChanged();
  });
  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (m_lockScreen.isActive()) {
      m_lockScreen.onKeyboardEvent(event);
      return;
    }
    // Grab popups are modal: while one is open it owns the keyboard and ESC
    // dismisses it before anything behind can react.
    if (ContextMenuPopup::dispatchKeyboardEvent(event)) {
      return;
    }
    if (m_trayMenu.onKeyboardEvent(event)) {
      return;
    }
    if (m_colorPickerDialogPopup.isOpen()) {
      m_colorPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_glyphPickerDialogPopup.isOpen()) {
      m_glyphPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_fileDialogPopup.isOpen()) {
      m_fileDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_searchPickerDialogPopup.isOpen()) {
      m_searchPickerDialogPopup.onKeyboardEvent(event);
      return;
    }
    if (m_screenshotService.onKeyboardEvent(event)) {
      return;
    }
    if (m_lockscreenWidgetsController.isEditing()) {
      m_lockscreenWidgetsController.onKeyboardEvent(event);
      return;
    }
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.onKeyboardEvent(event);
      return;
    }
    if (m_pluginLayerHost.onKeyboardEvent(event)) {
      return;
    }
    if (m_settingsWindow.ownsKeyboardSurface(m_wayland.lastKeyboardSurface())) {
      m_settingsWindow.onKeyboardEvent(event);
      return;
    }
    if (m_activityWindow.ownsKeyboardSurface(m_wayland.lastKeyboardSurface())) {
      m_activityWindow.onKeyboardEvent(event);
      return;
    }
    if (m_overviewLauncherCapture.handleKeyboardEvent(event)) {
      return;
    }
    if (m_notificationToast.onKeyboardEvent(event)) {
      return;
    }
    if (m_windowSwitcher.onKeyboardEvent(event)) {
      return;
    }
    m_panelManager.onKeyboardEvent(event);
  });
}

void Application::initPanelManagerAndPanels() {
  // Panel manager must be before bar so widgets can access PanelManager::instance()
  m_panelManager.initialize(m_compositorPlatform, &m_configService, &m_renderContext);
  m_panelManager.setOpenSettingsWindowCallback([this](std::string context) {
    m_settingsWindow.open(std::move(context));
  });
  m_panelManager.setCloseSettingsWindowCallback([this] { m_settingsWindow.close(); });
  m_panelManager.setToggleSettingsWindowCallback([this](std::string context) {
    if (m_settingsWindow.isOpen()) {
      m_settingsWindow.close();
      return;
    }
    m_settingsWindow.open(std::move(context));
  });
  m_panelManager.setCloseDesktopWidgetsEditorCallback([this] {
    if (m_desktopWidgetsController.isEditing()) {
      m_desktopWidgetsController.exitEdit();
    }
  });
  m_settingsWindow.setOpenWallpaperPanel([this] {
    wl_output* output = m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200));
    m_panelManager.openPanel("wallpaper", PanelOpenRequest{.output = output});
  });
  m_settingsWindow.setCalendarService(&m_calendarService);
  m_calendarService.setCredentialChangeCallback([this] {
    m_settingsWindow.onExternalOptionsChanged();
    retrySecretServiceConsumers();
  });
  m_settingsWindow.setClipboardService(&m_clipboardService);
  m_settingsWindow.setNotificationManager(&m_notificationManager);
  m_clipboardService.setPersistenceChangeCallback([this] { m_settingsWindow.onExternalOptionsChanged(); });
  m_settingsWindow.setResetEncryptedStorage([this] {
    const bool clipboardCleared = m_clipboardService.clearEncryptedPersistenceForRecovery();
    const bool calendarCleared = m_calendarService.clearEncryptedCacheForRecovery();
    const bool notificationsCleared = m_notificationManager.clearEncryptedHistoryForRecovery();
    if (!clipboardCleared || !calendarCleared || !notificationsCleared) {
      m_settingsWindow.showTransientStatus(i18n::tr("settings.storage-recovery.error-delete"), true);
      m_settingsWindow.onExternalOptionsChanged();
      return;
    }
    m_storageKeyProvider.resetAfterEncryptedDataCleared([this](bool success) {
      m_settingsWindow.showTransientStatus(
          i18n::tr(success ? "settings.storage-recovery.success" : "settings.storage-recovery.error-key"), !success
      );
      m_settingsWindow.onExternalOptionsChanged();
    });
  });
  auto clipboardPanel = std::make_unique<ClipboardPanel>(&m_clipboardService, &m_configService, &m_asyncTextureCache);
  clipboardPanel->setActivateCallback([this](const ClipboardEntry& entry) {
    const ClipboardAutoPasteMode mode = m_configService.config().shell.clipboardAutoPaste;
    if (mode == ClipboardAutoPasteMode::Off) {
      m_panelManager.close();
      return;
    }
    // Auto-paste injects a keystroke into whatever holds keyboard focus. The animated close keeps
    // the panel surface (and its keyboard focus) alive for the duration of the reveal animation, so
    // the keys would land on the closing panel instead of the target window. Close without animation
    // so focus returns to the toplevel before we paste, mirroring the launcher's app-launch close.
    m_panelManager.closePanel(false);
    const bool isImage = entry.isImage();
    m_clipboardAutoPasteTimer.stop();
    m_clipboardAutoPasteTimer.start(std::chrono::milliseconds(150), [this, isImage] {
      DeferredCall::callLater([this, isImage] {
        const ClipboardAutoPasteMode activeMode = m_configService.config().shell.clipboardAutoPaste;
        (void)clipboard_paste::pasteEntry(isImage, activeMode, m_virtualKeyboardService);
      });
    });
  });
  m_panelManager.registerPanel("clipboard", std::move(clipboardPanel));
  // The Spotlight launcher owns clipboard history as a mode (DESIGN.md "Launcher media modes").
  if (m_configService.config().shell.launcher.layout == ShellConfig::LauncherLayout::Full) {
    m_panelManager.setPanelAlias("clipboard", "launcher", "mode:Clipboard");
  }
  syncClipboardService();
  m_panelManager.registerPanel("session", std::make_unique<SessionPanel>(&m_configService, m_sessionActionRunner));
  m_panelManager.registerPanel("display-mode", std::make_unique<DisplayModePanel>(m_wayland));
  m_panelManager.registerPanel("test", std::make_unique<TestPanel>());
  m_panelManager.registerPanel("phone", std::make_unique<PhonePanel>(m_linkService.get()));
  auto controlCenterPanel = std::make_unique<ControlCenterPanel>(ControlCenterServices{
      .notifications = &m_notificationManager,
      .audio = pipewire(),
      .easyEffects = m_easyEffectsService.get(),
      .mpris = mpris(),
      .config = &m_configService,
      .httpClient = &m_httpClient,
      .weather = &m_weatherService,
      .spectrum = m_pipewireSpectrum.get(),
      .upower = upower(),
      .powerProfiles = powerProfiles(),
      .network = network(),
      .hotspot = m_hotspot.get(),
      .link = m_linkService.get(),
      .quickShare = m_quickShareService.get(),
      .screenRecorder = m_screenRecorder.get(),
      .modem = m_modemManagerService.get(),
      .networkSecrets = networkSecretAgent(),
      .externalIp = &m_externalIpService,
      .bluetooth = bluetooth(),
      .bluetoothAgent = bluetoothAgent(),
      .brightness = m_brightnessService.get(),
      .sysmon = m_systemMonitor.get(),
      .screenTime = &m_screenTimeService,
      .nightLight = &m_gammaService,
      .theme = &m_themeService,
      .idleInhibitor = &m_idleInhibitor,
      .dependencies = &m_dependencyService,
      .platform = &m_compositorPlatform,
      .ipc = &m_ipcService,
      .wallpaper = &m_wallpaper,
      .calendar = &m_calendarService,
      .scriptApi = &m_scriptApi,
      .fileWatcher = &m_fileWatcher,
      .clipboard = &m_clipboardService,
      .accounts = m_accountsService.get(),
      .thumbnails = &m_thumbnailService,
      .asyncTextures = &m_asyncTextureCache,
  });
  ControlCenterPanel* controlCenterPanelPtr = controlCenterPanel.get();
  m_panelManager.registerPanel("control-center", std::move(controlCenterPanel));
  {
    auto launcherPanel =
        std::make_unique<LauncherPanel>(&m_configService, m_accountsService.get(), &m_asyncTextureCache);
    launcherPanel->addProvider(std::make_unique<AppProvider>(&m_configService, &m_compositorPlatform));
    launcherPanel->addProvider(std::make_unique<WallpaperProvider>(&m_configService, &m_wayland, &m_themeService));
    launcherPanel->addProvider(std::make_unique<WindowProvider>(&m_compositorPlatform));
    launcherPanel->addProvider(
        std::make_unique<PanelProvider>(&m_panelManager, controlCenterPanelPtr, &m_configService)
    );
    launcherPanel->addProvider(std::make_unique<SessionProvider>(&m_configService, &m_sessionActionRunner));
    launcherPanel->addProvider(std::make_unique<WebSearchProvider>());
    launcherPanel->addProvider(std::make_unique<MathProvider>(&m_clipboardService, &m_configService, &m_httpClient));
    launcherPanel->addProvider(std::make_unique<EmojiProvider>(&m_clipboardService, &m_configService));
    launcherPanel->addProvider(std::make_unique<ClipboardProvider>(&m_clipboardService));
    launcherPanel->addProvider(std::make_unique<KlipyProvider>(
        KlipyProvider::Kind::Gifs, &m_configService, &m_httpClient, &m_clipboardService
    ));
    launcherPanel->addProvider(std::make_unique<KlipyProvider>(
        KlipyProvider::Kind::Stickers, &m_configService, &m_httpClient, &m_clipboardService
    ));
    launcherPanel->addProvider(std::make_unique<SymbolProvider>(&m_clipboardService));
    auto files = std::make_unique<FilesProvider>(&m_clipboardService, m_bus.get());
    m_filesProvider = files.get();
    launcherPanel->addProvider(std::move(files));
    launcherPanel->setCopiedActivationCallback([this] {
      const ClipboardAutoPasteMode mode = m_configService.config().shell.launcher.autoPaste;
      if (mode == ClipboardAutoPasteMode::Off) {
        return;
      }
      m_launcherAutoPasteTimer.stop();
      m_launcherAutoPasteTimer.start(std::chrono::milliseconds(150), [this] {
        DeferredCall::callLater([this] {
          const ClipboardAutoPasteMode activeMode = m_configService.config().shell.launcher.autoPaste;
          (void)clipboard_paste::pasteEntry(false, activeMode, m_virtualKeyboardService);
        });
      });
    });
    launcherPanel->setOnAppLaunchCallback([this] {
      if (!overviewCloseOnLaunchEnabled(m_configService)) {
        return;
      }
      (void)m_compositorPlatform.closeOverview();
    });
    m_launcherPanel = launcherPanel.get();
    m_panelManager.registerPanel("launcher", std::move(launcherPanel));
  }
  m_configService.addReloadCallback(
      [this] {
        if (m_launcherPanel != nullptr) {
          m_launcherPanel->syncUsageOrderingState();
        }
      },
      "launcher-usage"
  );
  m_settingsWindow.setResetLauncherUsage([this] {
    if (m_launcherPanel != nullptr) {
      m_launcherPanel->clearUsage();
    }
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.launcher-usage-reset"),
        i18n::tr("notifications.internal.launcher-usage-reset-success")
    );
  });
  m_settingsWindow.setResetScreenTime([this] {
    m_screenTimeService.clearAll();
    notify::info(
        "Noctalia", i18n::tr("notifications.internal.screen-time-reset"),
        i18n::tr("notifications.internal.screen-time-reset-success")
    );
  });
  reloadPluginLauncherProviders();
  reloadDmenuProviders();
  reloadPluginPanels();
  reloadPluginLayers();
  m_configService.addReloadCallback([this] { m_pluginLayerHost.onConfigReload(); });
  m_overviewLauncherCapture.initialize(m_wayland, &m_renderContext, m_compositorPlatform, m_panelManager);
  m_overviewLauncherCapture.setEnabled(overviewTypeToLaunchEnabled(m_configService));
  m_overviewLauncherCapture.setOpenLauncherCallback(
      [this](std::string_view initialQuery, wl_output* output, std::string_view sourceBarName) {
        if (m_panelManager.isOpenPanel("launcher")) {
          return;
        }
        m_panelManager.openPanel(
            "launcher", PanelOpenRequest{.output = output, .context = initialQuery, .sourceBarName = sourceBarName}
        );
      }
  );
  m_compositorPlatform.setOverviewChangeCallback([this] {
    m_overviewLauncherCapture.sync();
    m_bar.scheduleSmartAutoHideReevaluation();
    m_dock.scheduleSmartAutoHideReevaluation();
  });
  m_panelManager.setPanelOpenedCallback([this] {
    m_overviewLauncherCapture.sync();
    if (m_panelManager.isAttachedOpen()) {
      m_bar.revealAutoHideForAttachedPanel(
          m_panelManager.attachedPanelOutput(), m_panelManager.attachedSourceBarName()
      );
    }
  });
  m_panelManager.setPanelClosedCallback([this] {
    m_overviewLauncherCapture.sync();
    m_bar.rearmTooltipForHoveredWidget();
    m_bar.reevaluateAutoHide();
    // Widgets that stay visible while their panel is open re-evaluate on the next update.
    m_bar.refresh();
  });
  m_configService.addReloadCallback([this] {
    m_overviewLauncherCapture.setEnabled(overviewTypeToLaunchEnabled(m_configService));
  });
  m_overviewLauncherCapture.sync();
  m_panelManager.registerPanel(
      "wallpaper",
      std::make_unique<WallpaperPanel>(
          &m_wayland, &m_configService, &m_thumbnailService, &m_wallpaperScanner, &m_themeService
      )
  );
  m_panelManager.registerPanel("tray-drawer", std::make_unique<TrayDrawerPanel>(m_trayService.get(), &m_configService));
  m_panelManager.registerPanel("polkit", std::make_unique<PolkitPanel>(&m_configService, [this] {
                                 return m_polkitAgent.get();
                               }));
  m_panelManager.registerPanel("setup-wizard", std::make_unique<SetupWizardPanel>(&m_configService, &m_wayland));

  if (SetupWizardPanel::isFirstRun(m_configService)) {
    DeferredCall::callLater([] { PanelManager::instance().togglePanel("setup-wizard"); });
  }
}

void Application::initNotificationAndOsd() {
  m_notificationToast.initialize(
      m_wayland, m_compositorPlatform, &m_configService, &m_notificationManager, &m_renderContext, &m_httpClient
  );
  m_configService.addReloadCallback([this] { m_notificationToast.onConfigReload(); });
  auto applyNotificationFilterConfig = [this] {
    m_notificationManager.setFilters(m_configService.config().notification.filters);
  };
  auto applyHistoryRetention = [this] {
    m_notificationManager.setHistoryRetentionHours(m_configService.config().notification.historyRetentionHours);
  };
  auto applyDismissedHistory = [this] {
    m_notificationManager.setKeepDismissedInHistory(m_configService.config().notification.keepDismissedInHistory);
  };
  applyDismissedHistory();
  m_configService.addReloadCallback(applyDismissedHistory);
  applyHistoryRetention();
  m_configService.addReloadCallback(applyHistoryRetention);
  applyNotificationFilterConfig();
  m_configService.addReloadCallback(applyNotificationFilterConfig);
  m_configService.setNotificationManager(&m_notificationManager);
  m_notificationManager.setSoundPlayer(m_soundPlayer.get());

  // session/noctalia.service writes this marker when the previous run failed.
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && runtime[0] != '\0') {
    std::error_code ec;
    if (std::filesystem::remove(std::filesystem::path(runtime) / "noctalia-crashed", ec)) {
      m_notificationManager.addInternal(
          "Noctalia", i18n::tr("notifications.internal.shell-restarted"),
          i18n::tr("notifications.internal.shell-restarted-body")
      );
    }
  }

  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
  m_osdOverlay.initialize(m_wayland, m_compositorPlatform, &m_configService, &m_renderContext);
  m_liveActivity.initialize(m_wayland, &m_configService, &m_renderContext);
  m_liveActivity.setVisibilityCallback([this](wl_output* output, bool visible) { m_bar.setCenterIslandHidden(output, visible); });
  m_liveActivity.setSpaceProvider([this](wl_output* output) { return m_bar.centerIslandSpace(output); });
  m_liveActivity.setRestSources([this] { return pipewire(); }, [this] { return mpris(); }, &m_notificationManager, [this](wl_output* output) {
    m_panelManager.togglePanel("control-center", PanelOpenRequest{.output = output, .context = "notifications"});
  });
  m_liveActivity.setExtraSources(&m_httpClient, [this] { return m_pipewireSpectrum.get(); }, &m_calendarService);
  (void)m_calendarService.addChangeCallback(m_liveActivity.refresher());
  m_osdOverlay.setIsland(
      [this](const OsdContent& content) { return m_liveActivity.showOsd(content); },
      [this] { return m_liveActivity.osdVisible(); }
  );
  m_notificationToast.setIslandHook([this](const Notification& n, NotificationEvent event) {
    return m_liveActivity.showBanner(n, event);
  });
  m_bar.setCenterIslandSpaceCallback(m_liveActivity.refresher());
  m_configService.addReloadCallback(m_liveActivity.refresher());
  m_islandUnreadToken = m_notificationManager.addUnreadCountCallback(m_liveActivity.refresher());
  m_windowSwitcher.initialize(
      m_wayland, &m_renderContext, m_compositorPlatform, &m_configService, &m_asyncTextureCache
  );
  m_configService.addReloadCallback([this] { m_windowSwitcher.onConfigReload(); });
  m_configService.addReloadCallback([this] { m_osdOverlay.onConfigReload(); });
  m_scriptApi.setShowOsdHook(
      [this](const std::string& icon, const std::string& value, std::optional<double> progress, bool inactive) {
        m_osdOverlay.show(customOsdContent(icon, value, progress, inactive));
      }
  );
  m_idleGraceOverlay.initialize(m_wayland, &m_renderContext);
  m_pluginIdleWatches.initialize(m_wayland);
  m_wayland.setIdleCapabilitiesReadyCallback([this] {
    m_idleManager.reload(m_configService.config().idle);
    m_pluginIdleWatches.onCapabilitiesReady();
  });
  m_idleManager.initialize(
      m_wayland,
      [this](
          const std::string& behaviorName, std::chrono::milliseconds fadeIn, bool willLockSession,
          std::function<void()> onFadeComplete
      ) {
        (void)behaviorName;
        (void)willLockSession;
        const std::uint64_t generation = ++m_idleGraceOverlayGeneration;
        // Snapshot before the overlay fades in. A lock behavior can join an
        // already-active grace period after this callback has run.
        if (m_configService.isLockScreenEnabled()) {
          m_lockScreen.primeDesktopCaptures();
        }
        DeferredCall::callLater([this, generation, fadeIn, done = std::move(onFadeComplete)] mutable {
          if (generation != m_idleGraceOverlayGeneration) {
            return;
          }
          m_idleGraceOverlay.show(fadeIn, std::move(done));
        });
      },
      [this](bool userCancelled, bool willLockSession) {
        ++m_idleGraceOverlayGeneration;
        // Keep the overlay only when handing off to Noctalia's lock screen (avoids a flash).
        // External lockers never take ownership; deferred hide also races with suspend.
        const bool handoffToLockScreen = !userCancelled && willLockSession && m_configService.isLockScreenEnabled();
        if (!handoffToLockScreen) {
          m_idleGraceOverlay.hide();
          m_lockScreen.clearPrimedDesktopCaptures();
        }
      }
  );
  m_idleManager.setActionRunner(
      [this](const IdleBehaviorConfig& /*behavior*/, const IdleActionRequest& action) -> bool {
        return runIdleAction(action);
      }
  );
  m_idleManager.setLiveIdleChangeCallback([this] {
    DeferredCall::callLater([this] { m_settingsWindow.onIdleLiveStatusChanged(); });
  });
  m_idleManager.reload(m_configService.config().idle);
  m_screenSaverModule = m_services.add<ScreenSaverModule>(m_systemBus.get());
  if (!m_services.start(m_configService.config())) {
    kLog.error("service modules: {}", m_services.error());
  }
  if (screenSaver() != nullptr) {
    screenSaver()->setChangeCallback([this](std::int64_t locks) {
      m_idleManager.setScreenSaverInhibitLocks(locks);
      m_pluginIdleWatches.setScreenSaverInhibited(locks > 0);
    });
    m_idleManager.setScreenSaverInhibitLocks(screenSaver()->inhibitLocks());
    m_pluginIdleWatches.setScreenSaverInhibited(screenSaver()->inhibitLocks() > 0);
  } else if (!m_screenSaverModule->startError().empty()) {
    kLog.warn("idle inhibit service disabled: {}", m_screenSaverModule->startError());
  }
  m_configService.addReloadCallback(
      [this] {
        if (m_configService.lastChange().idle) {
          m_idleManager.reload(m_configService.config().idle);
        }
      },
      "idle"
  );
  m_audioOsd.bindOverlay(m_osdOverlay);
  m_audioOsd.setSoundPlayer(m_soundPlayer.get());
  m_screenshotService.setSoundPlayer(m_soundPlayer.get());
  if (pipewire() != nullptr) {
    m_audioOsd.primeFromService(*pipewire());
  }
  m_brightnessOsd.bindOverlay(m_osdOverlay);
  if (m_brightnessService != nullptr) {
    m_brightnessOsd.primeFromService(*m_brightnessService);
  }
  m_keyboardBacklightOsd.bindOverlay(m_osdOverlay);
  if constexpr (kLockKeysEnabled) {
    m_lockKeysOsd.bindOverlay(m_osdOverlay);
    m_lockKeysOsd.primeFromService(m_lockKeysService);
  }
  m_keyboardLayoutOsd.bindOverlay(m_osdOverlay);
  m_keyboardLayoutOsd.prime(m_compositorPlatform);
  m_mediaOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.bindOverlay(m_osdOverlay);
  m_privacyOsd.configure(m_configService.config());
  m_configService.addReloadCallback(
      [this] {
        if (m_configService.lastChange().shell) {
          m_privacyOsd.onConfigReload(m_configService.config(), pipewire());
          m_bar.refresh();
        }
      },
      "privacy-filters"
  );
}

void Application::initBarDockAndLayout() {
  m_trayMenu.initialize(m_wayland, &m_configService, m_trayService.get(), &m_renderContext);
  m_trayMenu.setClosedCallback([this] { m_bar.reevaluateAutoHideAfterPopup(); });

  m_bar.initialize({
      .platform = m_compositorPlatform,
      .config = m_configService,
      .notifications = &m_notificationManager,
      .tray = m_trayService.get(),
      .audio = pipewire(),
      .easyEffects = m_easyEffectsService.get(),
      .upower = upower(),
      .sysmon = m_systemMonitor.get(),
      .powerProfiles = powerProfiles(),
      .network = network(),
      .modem = m_modemManagerService.get(),
      .externalIp = &m_externalIpService,
      .idleInhibitor = &m_idleInhibitor,
      .mpris = mpris(),
      .audioSpectrum = m_pipewireSpectrum.get(),
      .httpClient = &m_httpClient,
      .weather = &m_weatherService,
      .renderContext = &m_renderContext,
      .nightLight = &m_gammaService,
      .theme = &m_themeService,
      .bluetooth = bluetooth(),
      .link = m_linkService.get(),
      .brightness = m_brightnessService.get(),
      .lockKeys = kLockKeysEnabled ? &m_lockKeysService : nullptr,
      .clipboard = &m_clipboardService,
      .fileWatcher = &m_fileWatcher,
      .screenshots = &m_screenshotService,
      .scriptApi = &m_scriptApi,
  });
  m_idleInhibitor.setAnchorSurfacesProvider([this] { return m_bar.caffeineAnchorSurfaces(); });
  m_panelManager.setOpenWidgetSettingsCallback([this](std::string barName, std::string widgetName) {
    m_settingsWindow.openToBarWidget(std::move(barName), std::move(widgetName));
  });
  m_panelManager.setOpenPluginSettingsCallback([this](std::string pluginId) {
    return m_settingsWindow.openToPlugin(std::move(pluginId));
  });
  m_panelManager.setAttachedPanelGeometryCallback(
      [this](wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry) {
        m_bar.setAttachedPanelGeometry(output, barName, geometry);
      }
  );
  m_panelManager.setClickShieldExcludeRectsProvider([this](wl_output* output) {
    return m_bar.surfaceRectsForOutput(output);
  });
  m_panelManager.setFocusGrabBarSurfacesProvider([this] { return m_bar.allBarSurfaces(); });
  m_panelManager.setAttachedPanelAvailabilityCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.canAttachPanelToBar(output, barName);
  });
  m_panelManager.setBarConfigProvider([this](wl_output* output, std::string_view barName) {
    return m_bar.configForBar(output, barName);
  });
  m_panelManager.setAttachedPanelBarSettledCallback([this](wl_output* output, std::string_view barName) {
    return m_bar.isAttachedPanelBarSettled(output, barName);
  });
  m_panelManager.setIslandSourceProvider([this](PanelOpenRequest& request, std::string_view panelId) {
    m_bar.fillIslandSource(request, panelId);
  });
  m_bar.setAutoHideSuppressionCallback([this](const BarInstance& instance) {
    if (m_trayMenu.isOpen()) {
      return true;
    }
    return m_panelManager.isAttachedOpen() && m_panelManager.attachedSourceBarName() == instance.barConfig.name;
  });
  // When config reloads, refresh any open panel: bar-driven attached decoration restyle and
  // shell-driven compositor blur.
  m_configService.addReloadCallback([this] { m_panelManager.onConfigReloaded(); });
  m_configService.addReloadCallback([this] {
    if (m_configService.lastChange().shell) {
      reloadDmenuProviders();
    }
  });
  m_configService.addReloadCallback([this] { m_screenCorners.onConfigReload(); });
  m_configService.addReloadCallback([this] { m_hotCorners.onConfigReload(); });

  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) {
        if (auto context = m_lockscreenWidgetsController.popupParentContextForSurface(surface); context.has_value()) {
          return context;
        }
        return m_desktopWidgetsController.popupParentContextForSurface(surface);
      },
      {}, {},
      [this] {
        if (auto context = m_lockscreenWidgetsController.fallbackPopupParentContext(); context.has_value()) {
          return context;
        }
        return m_desktopWidgetsController.fallbackPopupParentContext();
      }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_panelManager.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_panelManager.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_panelManager.endAttachedPopup(surface); },
      [this] { return m_panelManager.fallbackPopupParentContext(); }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_bar.popupParentContextForSurface(surface); },
      [this](wl_surface* surface) { m_bar.beginAttachedPopup(surface); },
      [this](wl_surface* surface) { m_bar.endAttachedPopup(surface); },
      [this] {
        return m_bar.preferredPopupParentContext(
            m_compositorPlatform.preferredInteractiveOutput(std::chrono::milliseconds(1200))
        );
      }
  );
  m_layerPopupHosts.registerHost(
      [this](wl_surface* surface) { return m_settingsWindow.popupParentContextForSurface(surface); }, {}, {},
      [this] { return m_settingsWindow.fallbackPopupParentContext(); }
  );

  m_colorPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);

  m_glyphPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);

  m_fileDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts, m_thumbnailService);
  m_searchPickerDialogPopup.initialize(m_wayland, m_configService, m_renderContext, m_layerPopupHosts);
  SearchPickerDialog::setPopup(&m_searchPickerDialogPopup);
  m_settingsWindow.initializeDialogPresenter(
      m_colorPickerDialogPopup, m_glyphPickerDialogPopup, m_fileDialogPopup, m_thumbnailService
  );
  ColorPickerDialog::setPresenter(m_settingsWindow.colorPickerDialogPresenter());
  GlyphPickerDialog::setPresenter(m_settingsWindow.glyphPickerDialogPresenter());
  FileDialog::setPresenter(m_settingsWindow.fileDialogPresenter());
}

void Application::initWidgetControllersAndCallbacks() {
  auto shouldRefreshControlCenter = [this] { return m_panelManager.isOpenPanel("control-center"); };

  m_dock.initialize(m_compositorPlatform, &m_configService, &m_renderContext, &m_notificationManager);
  const DesktopWidgetScriptDeps desktopWidgetScriptDeps{
      .scriptApi = &m_scriptApi,
      .fileWatcher = &m_fileWatcher,
      .clipboard = &m_clipboardService,
      .configService = &m_configService,
  };
  const DesktopWidgetRuntimeServices desktopWidgetRuntime{
      .calendar = &m_calendarService,
      .pipewire = pipewire(),
      .pipewireSpectrum = m_pipewireSpectrum.get(),
      .weather = &m_weatherService,
      .mpris = mpris(),
      .httpClient = &m_httpClient,
      .sysmon = m_systemMonitor.get(),
      .scriptDeps = desktopWidgetScriptDeps,
  };
  const DesktopWidgetServices lockscreenWidgetServices{
      .wayland = m_wayland,
      .config = &m_configService,
      .renderContext = &m_renderContext,
      .runtime = desktopWidgetRuntime,
      .textureCache = &m_sharedTextureCache,
  };
  const DesktopWidgetServices desktopWidgetServices{
      .wayland = m_wayland,
      .config = &m_configService,
      .renderContext = &m_renderContext,
      .runtime = desktopWidgetRuntime,
      .textureCache = &m_sharedTextureCache,
  };
  m_lockscreenWidgetsController.initialize({
      .widgets = lockscreenWidgetServices,
      .lockScreen = m_lockScreen,
      .bar = m_bar,
      .dock = m_dock,
      .desktopWidgets = &m_desktopWidgetsController,
  });
  m_desktopWidgetsController.initialize({
      .widgets = desktopWidgetServices,
      .lockscreenWidgets = &m_lockscreenWidgetsController,
  });
  m_desktopWidgetsController.setOnEnterEditCallback([this] { m_settingsWindow.closeForWidgetEditor(); });
  m_lockscreenWidgetsController.setOnEnterEditCallback([this] { m_settingsWindow.closeForWidgetEditor(); });
  const auto restoreSettingsAfterWidgetEditor = [this] {
    DeferredCall::callLater([this] {
      if (!m_desktopWidgetsController.isEditing() && !m_lockscreenWidgetsController.isEditing()) {
        m_settingsWindow.reopenAfterWidgetEditor();
      }
    });
  };
  m_desktopWidgetsController.setOnExitEditCallback(restoreSettingsAfterWidgetEditor);
  m_lockscreenWidgetsController.setOnExitEditCallback(restoreSettingsAfterWidgetEditor);
  m_iconThemePollSource.setChangeCallback([this] { onIconThemeChanged(); });

  std::string lastShellFontFamily = m_configService.config().shell.fontFamily;
  m_configService.addReloadCallback(
      [this, lastShellFontFamily] mutable {
        const std::string& newShellFontFamily = m_configService.config().shell.fontFamily;
        if (newShellFontFamily == lastShellFontFamily) {
          return;
        }

        lastShellFontFamily = newShellFontFamily;
        text::invalidateFontWeightCatalogCache();
        m_renderContext.setTextFontFamily(newShellFontFamily);
        m_bar.reload();
        m_dock.requestLayout();
        m_desktopWidgetsController.requestLayout();
        m_lockscreenWidgetsController.requestLayout();
        m_panelManager.requestLayout();
        m_notificationToast.requestLayout();
        m_lockScreen.onFontChanged();
        m_osdOverlay.requestLayout();
        m_trayMenu.onFontChanged();
        m_backdrop.onFontChanged();
        m_settingsWindow.onFontChanged();
        m_colorPickerDialogPopup.requestLayout();
        m_glyphPickerDialogPopup.requestLayout();
        m_fileDialogPopup.requestLayout();
        m_searchPickerDialogPopup.requestLayout();
        scheduleGreeterAutoSync();
      },
      "shell-font-family"
  );

  bool lastRtl = i18n::Service::instance().rtl();
  m_configService.addReloadCallback(
      [this, lastRtl] mutable {
        const bool rtl = i18n::Service::instance().rtl();
        if (rtl == lastRtl) {
          return;
        }

        lastRtl = rtl;
        Style::setRtl(rtl);
        m_renderContext.setTextBaseDirection(rtl);
        m_bar.requestLayout();
        m_dock.requestLayout();
        m_desktopWidgetsController.requestLayout();
        m_lockscreenWidgetsController.requestLayout();
        m_panelManager.requestLayout();
        m_notificationToast.requestLayout();
        m_lockScreen.onFontChanged();
        m_osdOverlay.requestLayout();
        m_trayMenu.onFontChanged();
        m_backdrop.onFontChanged();
        m_settingsWindow.onFontChanged();
        m_colorPickerDialogPopup.requestLayout();
        m_glyphPickerDialogPopup.requestLayout();
        m_fileDialogPopup.requestLayout();
        m_searchPickerDialogPopup.requestLayout();
        scheduleGreeterAutoSync();
      },
      "shell-language-direction"
  );

  m_timeService.setTickCallback([this] {
    m_liveActivity.onTimeTick();
    m_wallpaper.onSecondTick();
    if (m_lockScreen.isActive()) {
      m_lockscreenWidgetsController.onSecondTick();
    } else {
      m_bar.onSecondTick();
      m_desktopWidgetsController.onSecondTick();
      m_lockscreenWidgetsController.onSecondTick();
      m_settingsWindow.onSecondTick();
    }
  });
  m_timeService.setPrecisionProvider([this] { return timePrecisionNeeded(); });
  m_timeService.setDeadlineProvider([this] { return m_wallpaper.nextAutomationDeadline(); });

  if (pipewire() != nullptr) {
    m_audioOsd.suppressFor(std::chrono::milliseconds(2000));
    pipewire()->setChangeCallback([this, shouldRefreshControlCenter] {
      if (m_pipewireSpectrum != nullptr) {
        m_pipewireSpectrum->handleAudioStateChanged();
      }
      m_bar.refresh();
      m_liveActivity.refresh();
      m_settingsWindow.onAudioChanged();
      m_desktopWidgetsController.requestUpdate();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
      if (pipewire() != nullptr) {
        m_hdmiAudioSwitch.onAudioStateChanged(*pipewire(), m_configService.config().audio.autoSwitchHdmi);
        m_audioOsd.onAudioStateChanged(*pipewire());
        m_privacyOsd.onPrivacyStateChanged(*pipewire());
      }
    });
    pipewire()->setVolumePreviewCallback([this](bool isInput, std::uint32_t id, float volume, bool muted) {
      if (isInput) {
        m_audioOsd.showInput(id, volume, muted);
      } else {
        m_audioOsd.showOutput(id, volume, muted);
      }
    });
  }
  if (m_easyEffectsService != nullptr) {
    m_easyEffectsService->setChangeCallback([this, shouldRefreshControlCenter] {
      m_bar.refresh();
      if (shouldRefreshControlCenter()) {
        m_panelManager.refresh();
      }
    });
  }

  // Wire the corner surface owners here alongside the dock. Surface creation and
  // stacking order live entirely in reconcileOutputSurfaces(): screen corners and
  // the hot-corner trigger zones are built after the bar and dock so they are
  // never occluded by shell chrome on their shared layer.
  m_screenCorners.initialize(m_wayland, &m_configService, &m_renderContext);
  m_hotCorners.initialize(m_wayland, &m_configService, &m_renderContext);
  m_liveAppearance.setOnExternalScheme([this](std::string_view scheme) {
    m_configService.setThemeMode(scheme == "light" ? ThemeMode::Light : ThemeMode::Dark);
  });
  m_liveAppearance.initialize(m_wayland, [this](const LiveAppearance& appearance) {
    kLog.info(
        "live appearance: glass={:.2f} reduce_transparency={} increase_contrast={} reduce_motion={} accent={}",
        appearance.glass, appearance.reduceTransparency, appearance.increaseContrast, appearance.reduceMotion,
        appearance.accent
    );
    Style::setGlass(appearance.glass, appearance.reduceTransparency);
    MotionService::instance().setEnabled(m_configService.config().shell.animation.enabled);
    MotionService::instance().setReduced(appearance.reduceMotion);
    m_themeService.setLiveAppearance(appearance.accent, appearance.increaseContrast);
    if (m_settingsPortal != nullptr) {
      const Color accent = colorForRole(ColorRole::Accent, 1.0F);
      m_settingsPortal->setAccent(accent.r, accent.g, accent.b);
      m_settingsPortal->setHighContrast(appearance.increaseContrast);
      m_settingsPortal->setReducedMotion(appearance.reduceMotion);
    }
    if (m_configService.config().theme.toolkitThemes) {
      const auto& toolkit = m_themeService.toolkitColors();
      toolkit_theme::write(m_themeService.resolvedMode(), toolkit.accent, toolkit.accentText, toolkit.onAccent);
    } else {
      toolkit_theme::remove();
    }
    // Bar and dock re-apply their material fills on this signal; the other glass surfaces re-read them on reload.
    paletteChanged().emit();
    m_panelManager.onConfigReloaded();
    m_notificationToast.onConfigReload();
    m_osdOverlay.onConfigReload();
    requestAllSurfacesRedraw();
  });
}

TimeService::Precision Application::timePrecisionNeeded() const {
  // Only a visible clock that renders seconds needs the finer boundary. The
  // bar, desktop, and lockscreen hosts each report only their live widgets.
  if (m_bar.wantsSecondTicks()
      || m_desktopWidgetsController.wantsSecondTicks()
      || m_lockscreenWidgetsController.wantsSecondTicks()
      || m_settingsWindow.wantsSecondTicks()) {
    return TimeService::Precision::Second;
  }
  // Minute is the idle floor: it settles minute-aligned clocks. Wallpaper
  // automation wakes through its own deadline instead.
  return TimeService::Precision::Minute;
}

void Application::syncLockStreamPause() {
  const PowerProfilesService* profiles = powerProfiles();
  m_lockscreenWidgetsController.setStreamsPaused(
      profiles != nullptr && profiles->activeProfile() == "power-saver", m_compositorPlatform.outputPowerStates()
  );
}
