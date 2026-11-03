#pragma once

#include "app/deferred_call_poll_source.h"
#include "app/modules/bluetooth_module.h"
#include "app/modules/mpris_module.h"
#include "app/modules/network_module.h"
#include "app/modules/pipewire_module.h"
#include "app/modules/power_modules.h"
#include "app/modules/ui_modules.h"
#include "app/service_host.h"
#include "app/timer_poll_source.h"
#include "calendar/calendar_poll_source.h"
#include "calendar/calendar_reminder_monitor.h"
#include "calendar/calendar_reminder_poll_source.h"
#include "calendar/calendar_service.h"
#include "capture/screenshot_service.h"
#include "compositors/compositor_platform.h"
#include "compositors/workspace_alert_service.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/files/file_watcher.h"
#include "core/process/process_poll_source.h"
#include "core/timer_manager.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/notification/notification_poll_source.h"
#include "dbus/secret/secret_collection_probe.h"
#include "hooks/battery_hook_state.h"
#include "hooks/hook_manager.h"
#include "idle/idle_grace_overlay.h"
#include "idle/idle_inhibitor.h"
#include "idle/idle_manager.h"
#include "idle/plugin_idle_watches.h"
#include "ipc/ipc_poll_source.h"
#include "ipc/ipc_service.h"
#include "ipc/pixel_stream_service.h"
#include "launcher/dmenu_ipc.h"
#include "net/http_client.h"
#include "net/http_client_poll_source.h"
#include "notification/notification_manager.h"
#include "pipewire/hdmi_audio_switch.h"
#include "render/core/async_texture_cache.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/thumbnail_service.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_recommendations.h"
#include "scripting/plugin_service_host.h"
#include "scripting/script_api_context.h"
#include "security/secret_store.h"
#include "security/storage_key_provider.h"
#include "shell/backdrop/backdrop.h"
#include "shell/bar/bar.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/dock/dock.h"
#include "shell/hot_corners/hot_corners.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lockscreen_widgets_controller.h"
#include "shell/notification/notification_toast.h"
#include "shell/osd/audio_osd.h"
#include "shell/osd/brightness_osd.h"
#include "shell/osd/keyboard_backlight_osd.h"
#include "shell/osd/keyboard_layout_osd.h"
#include "shell/osd/lock_keys_osd.h"
#include "shell/osd/media_osd.h"
#include "shell/osd/osd_overlay.h"
#include "shell/live_activity/live_activity_island.h"
#include "shell/osd/privacy_osd.h"
#include "shell/overview/overview_launcher_capture.h"
#include "shell/panel/panel_manager.h"
#include "shell/plugin_layer/plugin_layer_host.h"
#include "shell/screen_corners/screen_corners.h"
#include "shell/session/session_action_runner.h"
#include "shell/settings/settings_window.h"
#include "shell/switcher/window_switcher.h"
#include "shell/tray/tray_menu.h"
#include "shell/wallpaper/panel/wallpaper_scanner.h"
#include "shell/wallpaper/wallpaper.h"
#include "shell/activity/activity_window.h"
#include "system/activity_actions.h"
#include "system/activity_monitor.h"
#include "system/background_activity_monitor.h"
#include "system/background_warning_state.h"
#include "system/battery_warning_monitor.h"
#include "system/dependency_service.h"
#include "system/desktop_entry_poll_source.h"
#include "system/gamma_service.h"
#include "system/icon_theme_poll_source.h"
#include "system/location_poll_source.h"
#include "system/location_service.h"
#include "system/lock_keys_poll_source.h"
#include "system/lock_keys_service.h"
#include "system/screen_time_service.h"
#include "system/telemetry_service.h"
#include "system/weather_poll_source.h"
#include "system/weather_service.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/template_apply_service.h"
#include "theme/live_appearance_sync.h"
#include "theme/theme_service.h"
#include "time/time_poll_source.h"
#include "time/time_service.h"
#include "ui/dialogs/color_picker_dialog_popup.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/dialogs/glyph_picker_dialog_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "ui/dialogs/search_picker_popup.h"
#include "clipboard/clipboard_poll_source.h"
#include "clipboard/clipboard_service.h"
#include "wayland/key_repeat_poll_source.h"
#include "compositors/keyboard_layout_poll_source.h"
#include "wayland/text_input_service.h"
#include "compositors/virtual_keyboard_service.h"
#include "compositors/shell_wayland_connection.h"
#include "compositors/workspace_poll_source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace sdbus {
  class IProxy;
}

class LauncherPanel;
class AccountsService;
class BluetoothAgent;
class BluetoothService;
class BrightnessPollSource;
class BrightnessService;
class KeyboardBacklightService;
class DebugService;
class AndromedaShellService;
class EasyEffectsService;
class INetworkService;
class IwdSecretAgent;
class LogindService;
class PowerConfigService;
class MainLoop;
class ModemManagerService;
class MprisService;
class NetworkSecretAgent;
class NotificationDBusHost;
class PipeWirePollSource;
class PipeWireService;
class WirePlumberMixer;
namespace compositors::andromeda {
  class AndromedaAudioProvider;
}
class PipeWireSpectrum;
class PipeWireSpectrumPollSource;
class PolkitAgent;
class PolkitPollSource;
class PowerProfilesService;
class ScreenSaverPollSource;
class ScreenSaverService;
class FilesProvider;
class SessionBus;
class SessionBusPollSource;
class SoundPlayer;
class SystemBus;
class SystemBusPollSource;
class SystemMonitorService;
class TrayService;
class UPowerService;
class UDisksService;
class GlobalShortcutsPortal;
class InhibitPortal;
class ScreenRecorder;
class NmHotspot;
class LinkService;
class QuickShareService;
class SettingsPortal;
enum class BluetoothStateChangeOrigin : std::uint8_t;
enum class NetworkChangeOrigin : std::uint8_t;
enum class PowerProfilesChangeOrigin : std::uint8_t;
struct BluetoothState;
struct NetworkState;
struct PowerProfilesState;

class Application {
public:
  Application();
  ~Application();

  void run(const std::function<void()>& startupReadyCallback = {});

  // Public for signal handler
  static std::atomic<bool> s_shutdownRequested;

  bool runShellCommand(const std::string& command);
  void triggerShellAction(const std::string& action, wl_output* output = nullptr);
  // Highest layer-shell layer occupied by any bar on the given output. Hot
  // corners place their trigger surfaces on this layer.
  [[nodiscard]] LayerShellLayer hotCornerLayerForOutput(wl_output* output) const noexcept;

private:
  void initServices();
  // Sub-phases of initServices(), called in order.
  void initStyleThemeAndWayland();
  void initWaylandCallbacks();
  void initAuxServicesAndHooks();
  void initSystemBusServices();
  void initBrightnessAndPipewire();
  void initEarlySessionBusAndTray();
  void initSessionBusServices();
  void initUi();
  ActivityMonitor* ensureActivityMonitor();
  void initActivity();
  // Sub-phases of initUi(), called in order.
  void initUiRenderSurfacesAndSettings();
  void initLockScreenAndSession();
  void initInputDispatch();
  void initPanelManagerAndPanels();
  void initNotificationAndOsd();
  void initBarDockAndLayout();
  void initWidgetControllersAndCallbacks();
  // Single source of truth for surface (re)creation order: (re)builds every
  // per-output layer surface bottom-to-top. Called once after initUi() wiring
  // and on every output change so first-run stacking matches hot reload.
  void reconcileOutputSurfaces();
  void initIpc();
  // (Re)register plugin-backed launcher providers from the enabled plugin set.
  void reloadPluginLauncherProviders();
  // (Re)register config-driven dmenu launcher providers ([shell.launcher.dmenu.entry.*]).
  void reloadDmenuProviders();
  // (Re)register plugin-backed panels from the enabled plugin set.
  void reloadPluginPanels();
  // Re-register plugin-backed [[layer]] entries from the enabled plugin set.
  void reloadPluginLayers();
  // When [plugins].auto_update is on, pull git sources per the configured mode. Run once at
  // startup and on a 6h repeating timer so long-lived sessions pick up new plugin versions.
  void runPluginAutoUpdate();
  void startTrayService();
  void syncNotificationDaemon();
  void installNotificationBusNameWatch();
  // gnome-keyring / kwalletd usually claim org.freedesktop.secrets a moment after Noctalia starts,
  // so the first credential lookups report "no provider". Watch the bus name and re-drive the
  // consumers that gave up once an owner appears.
  void installSecretServiceNameWatch();
  // The provider may be present but its collection still locked at startup (PAM holds the password
  // but only opens the store on first request; or a fingerprint/autologin session unlocks it a few
  // seconds later). Watch the collection set and re-drive consumers once the default collection is
  // actually unlocked, so a lookup that lost the startup race recovers without restarting Noctalia.
  void installSecretServiceCollectionWatch();
  void onSecretServiceCollectionChanged();
  void retrySecretServiceConsumers(bool defaultCollectionUnlocked = false);
  void scheduleNotificationShellRefresh();
  void syncLiveActivity();
  void syncPolkitAgent();
  [[nodiscard]] bool likelySupportsInSessionPolkit() const noexcept;
  void syncClipboardService();
  void syncStorageKeyProvider();
  void syncScreenTimeService();
  void performGreeterSync(bool quiet = false);
  void scheduleGreeterAutoSync();
  bool runShellCommandBlocking(const std::string& command);
  bool runIdleAction(const IdleActionRequest& action);
  bool setOutputPower(bool on);
  // Lock helper slots stop under the power-saver profile and on outputs that are off.
  void syncLockStreamPause();
  void onIconThemeChanged();
  // Finest time boundary the currently visible shell UI needs. Drives the time
  // poll source so it can park between boundaries instead of waking every second.
  [[nodiscard]] TimeService::Precision timePrecisionNeeded() const;
  void onGraphicsReset(RenderGraphicsResetStatus status);
  void recoverGraphicsAfterReset();
  void requestAllSurfacesRedraw();
  void releaseSleepDelayInhibitIfPending();
  void onUpowerStateChangedForHooks();
  // Battery and network for the lock screen's status island.
  void refreshLockStatus();
  void applyPowerSourceProfile();
  [[nodiscard]] UPowerService* upower() const noexcept {
    return m_upowerModule != nullptr ? m_upowerModule->service() : nullptr;
  }
  [[nodiscard]] PowerProfilesService* powerProfiles() const noexcept {
    return m_powerProfilesModule != nullptr ? m_powerProfilesModule->service() : nullptr;
  }
  [[nodiscard]] INetworkService* network() const noexcept {
    return m_networkModule != nullptr ? m_networkModule->service() : nullptr;
  }
  [[nodiscard]] NetworkSecretAgent* networkSecretAgent() const noexcept {
    return m_networkModule != nullptr ? m_networkModule->secretAgent() : nullptr;
  }
  [[nodiscard]] BluetoothService* bluetooth() const noexcept {
    return m_bluetoothModule != nullptr ? m_bluetoothModule->service() : nullptr;
  }
  [[nodiscard]] MprisService* mpris() const noexcept {
    return m_mprisModule != nullptr ? m_mprisModule->service() : nullptr;
  }
  [[nodiscard]] PipeWireService* pipewire() const noexcept {
    return m_pipewireModule != nullptr ? m_pipewireModule->service() : nullptr;
  }
  [[nodiscard]] ScreenSaverService* screenSaver() const noexcept {
    return m_screenSaverModule != nullptr ? m_screenSaverModule->service() : nullptr;
  }
  [[nodiscard]] BluetoothAgent* bluetoothAgent() const noexcept {
    return m_bluetoothModule != nullptr ? m_bluetoothModule->agent() : nullptr;
  }
  void onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin);
  void onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin);
  void onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin);
  [[nodiscard]] std::vector<PollSource*> currentPollSources();
  [[nodiscard]] std::vector<PollSource*> buildPollSources();

  ShellWaylandConnection m_wayland;
  WorkspaceAlertService m_workspaceAlertService;
  CompositorPlatform m_compositorPlatform{m_wayland};
  security::SecretStore m_secretStore;
  security::StorageKeyProvider m_storageKeyProvider{m_secretStore};
  ClipboardService m_clipboardService{m_storageKeyProvider};
  TextInputService m_textInputService;
  VirtualKeyboardService m_virtualKeyboardService;
  ConfigService m_configService;
  HttpClient m_httpClient;
  FileWatcher m_fileWatcher;
  noctalia::theme::CommunityPaletteService m_communityPaletteService{m_httpClient};
  noctalia::theme::CommunityTemplateService m_communityTemplateService{m_httpClient};
  noctalia::theme::ThemeService m_themeService{m_configService, m_httpClient};
  LiveAppearanceSync m_liveAppearance;
  noctalia::theme::TemplateApplyService m_templateApplyService{m_configService};
  scripting::ScriptApiContext m_scriptApi;
  std::function<void()> m_syncScriptApiOutputs;
  scripting::PluginManager m_pluginManager{m_configService};
  scripting::PluginRecommendations m_pluginRecommendations{m_configService, m_httpClient, m_pluginManager};
  scripting::PluginServiceHost m_pluginServiceHost{m_scriptApi, &m_httpClient, &m_clipboardService, &m_fileWatcher};
  TimeService m_timeService;
  LockKeysService m_lockKeysService;
  NotificationManager m_notificationManager;
  CalendarService m_calendarService;
  CalendarReminderMonitor m_calendarReminderMonitor{m_configService, m_notificationManager};
  std::unique_ptr<SessionBus> m_bus;
  // Set when the early session bus connection fails. Reported once i18n has been
  // initialized in initStyleThemeAndWayland().
  std::optional<std::string> m_earlySessionBusError;
  std::unique_ptr<SystemBus> m_systemBus;
  std::unique_ptr<LogindService> m_logindService;
  std::unique_ptr<PowerConfigService> m_powerConfigService;
  // Set on PrepareForSleep(true); cleared when the session lock engages (or the lock aborts).
  bool m_releaseSleepDelayWhenLocked = false;
  // Set before Noctalia-initiated suspend so PrepareForSleep skips lock-before-sleep.
  bool m_skipLockOnNextSleep = false;
  std::uint64_t m_outputPowerRequestGeneration = 0;
  std::unique_ptr<AccountsService> m_accountsService;
  std::unique_ptr<ScreenSaverPollSource> m_screenSaverPollSource;
  std::unique_ptr<SystemMonitorService> m_systemMonitor;
  std::unique_ptr<DebugService> m_debugService;
  std::unique_ptr<AndromedaShellService> m_andromedaShellService;
  IdleInhibitor m_idleInhibitor;
  // Created on the first Activity request; samples only under a lease.
  std::unique_ptr<ActivityMonitor> m_activityMonitor;
  std::unique_ptr<ActivityActions> m_activityActions;
  // Outlives the window: 24 h memory history fills from the background check.
  ActivityDetails m_activityDetails;
  // Declared after the monitor: it releases its monitor reference when it closes.
  ActivityWindow m_activityWindow;
  std::unique_ptr<DebugService> m_debugService;
  std::unique_ptr<AndromedaShellService> m_andromedaShellService;
  IdleInhibitor m_idleInhibitor;
  std::unique_ptr<BackgroundWarningState> m_backgroundWarningState;
  std::unique_ptr<BackgroundActivityMonitor> m_backgroundActivityMonitor;
  IdleManager m_idleManager;
  PluginIdleWatches m_pluginIdleWatches;
  IdleGraceOverlay m_idleGraceOverlay;
  std::uint64_t m_idleGraceOverlayGeneration = 0;
  HookManager m_hookManager;
  DependencyService m_dependencyService;
  GammaService m_gammaService;
  ScreenshotService m_screenshotService{
      m_wayland, m_compositorPlatform, m_configService, m_notificationManager, &m_clipboardService
  };
  // Declared before m_services: PipeWireService points to it and must be destroyed first.
  std::unique_ptr<WirePlumberMixer> m_wirePlumberMixer;
  // Owns the moved service modules (docs/m0-foundation.md §4); stops them in reverse start order.
  ServiceHost m_services;
  PowerProfilesModule* m_powerProfilesModule = nullptr;
  UPowerModule* m_upowerModule = nullptr;
  NetworkModule* m_networkModule = nullptr;
  BluetoothModule* m_bluetoothModule = nullptr;
  MprisModule* m_mprisModule = nullptr;
  PipeWireModule* m_pipewireModule = nullptr;
  ScreenSaverModule* m_screenSaverModule = nullptr;
  std::optional<bool> m_profileAppliedOnBattery;
  ExternalIpService m_externalIpService{&m_httpClient, &m_configService};
  std::unique_ptr<UDisksService> m_udisksService;
  std::unique_ptr<SettingsPortal> m_settingsPortal;
  std::unique_ptr<InhibitPortal> m_inhibitPortal;
  std::unique_ptr<GlobalShortcutsPortal> m_globalShortcutsPortal;
  std::unique_ptr<NmHotspot> m_hotspot;
  std::unique_ptr<LinkService> m_linkService;
  std::unique_ptr<QuickShareService> m_quickShareService;
  std::unique_ptr<ScreenRecorder> m_screenRecorder;
  std::map<std::uint32_t, std::pair<std::string, std::string>> m_driveNotifications;
  std::unique_ptr<ModemManagerService> m_modemManagerService;
  Timer m_bluetoothResumeTimer;
  std::unique_ptr<PolkitAgent> m_polkitAgent;
  std::optional<bool> m_notificationDaemonEnabled;
  bool m_notificationDaemonInitFailed = false;
  bool m_notificationShellRefreshScheduled = false;
  BatteryHookState m_batteryHookState;
  BatteryWarningMonitor m_batteryWarningMonitor;
  std::optional<bool> m_prevBatteryPluggedForEvents;
  std::optional<bool> m_prevWirelessEnabledForEvents;
  std::optional<bool> m_prevBluetoothPoweredForEvents;
  std::optional<std::string> m_prevPowerProfileActiveForEvents;
  std::unique_ptr<BrightnessService> m_brightnessService;
  std::unique_ptr<KeyboardBacklightService> m_keyboardBacklightService;
  std::unique_ptr<TrayService> m_trayService;
  std::unique_ptr<NotificationDBusHost> m_notificationDbus;
  std::unique_ptr<sdbus::IProxy> m_notificationBusNameWatchProxy;
  bool m_notificationBusNameWatchInstalled = false;
  std::unique_ptr<sdbus::IProxy> m_secretServiceNameWatchProxy;
  bool m_secretServiceNameWatchInstalled = false;
  std::unique_ptr<sdbus::IProxy> m_secretServiceCollectionWatchProxy;
  std::unique_ptr<SecretCollectionProbe> m_secretServiceCollectionProbe;
  bool m_secretServiceCollectionWatchInstalled = false;
  bool m_secretServiceOwned = false;
  bool m_storageKeyAutoRetried = false;
  bool m_calendarCredentialAutoRetried = false;
  std::unique_ptr<EasyEffectsService> m_easyEffectsService;
  std::unique_ptr<PipeWireSpectrum> m_pipewireSpectrum;
  std::unique_ptr<compositors::andromeda::AndromedaAudioProvider> m_andromedaAudioProvider;
  std::uint64_t m_andromedaAudioListener = 0;
  std::shared_ptr<SoundPlayer> m_soundPlayer;

  TelemetryService m_telemetryService;
  ScreenTimeService m_screenTimeService;

  GlSharedContext m_glShared;
  // Pending m_glShared.initialize() on a worker thread; destroyed before m_glShared, so it waits for the worker.
  std::future<void> m_glSharedInit;
  SharedTextureCache m_sharedTextureCache;
  RenderContext m_renderContext;
  // Image controls in panels retain cache-backed handles during teardown.
  AsyncTextureCache m_asyncTextureCache;
  // Outlives the panels, widgets, and layers whose pixel stream views register with it.
  PixelStreamService m_pixelStreamService;
  ThumbnailService m_thumbnailService;
  WallpaperScanner m_wallpaperScanner;
  Bar m_bar;
  Dock m_dock;
  DesktopWidgetsController m_desktopWidgetsController;
  LockScreen m_lockScreen;
  LockscreenWidgetsController m_lockscreenWidgetsController;
  SessionActionRunner m_sessionActionRunner{m_compositorPlatform, m_lockScreen};
  PanelManager m_panelManager;
  // Owned by m_panelManager; kept raw so plugin launcher providers can be re-applied.
  LauncherPanel* m_launcherPanel = nullptr;
  // Owned by the launcher panel; polled for index daemon answers.
  FilesProvider* m_filesProvider = nullptr;
  // Ids of plugin-backed panels currently registered with m_panelManager, so a
  // reload can retire the previous set before registering the new one.
  std::vector<std::string> m_pluginPanelIds;
  PluginLayerHost m_pluginLayerHost{
      PluginLayerServices{
          .platform = m_compositorPlatform, .renderContext = &m_renderContext, .config = &m_configService
      },
      m_scriptApi, &m_fileWatcher, &m_httpClient, &m_clipboardService
  };
  WindowSwitcher m_windowSwitcher;
  OverviewLauncherCapture m_overviewLauncherCapture;
  NotificationToast m_notificationToast;
  AudioOsd m_audioOsd;
  HdmiAudioSwitch m_hdmiAudioSwitch;
  BrightnessOsd m_brightnessOsd;
  KeyboardBacklightOsd m_keyboardBacklightOsd;
  MediaOsd m_mediaOsd;
  LockKeysOsd m_lockKeysOsd;
  KeyboardLayoutOsd m_keyboardLayoutOsd;
  PrivacyOsd m_privacyOsd;
  OsdOverlay m_osdOverlay;
  LiveActivityIsland m_liveActivity;
  int m_islandUnreadToken = -1;
  HotCorners m_hotCorners{this};
  ScreenCorners m_screenCorners;
  TrayMenu m_trayMenu;
  Wallpaper m_wallpaper;
  Backdrop m_backdrop;
  SettingsWindow m_settingsWindow;
  LayerPopupHostRegistry m_layerPopupHosts;
  ColorPickerDialogPopup m_colorPickerDialogPopup;
  GlyphPickerDialogPopup m_glyphPickerDialogPopup;
  FileDialogPopup m_fileDialogPopup;
  SearchPickerPopup m_searchPickerDialogPopup;

  // Poll sources (must outlive MainLoop)
  std::unique_ptr<SessionBusPollSource> m_busPollSource;
  std::unique_ptr<SystemBusPollSource> m_systemBusPollSource;
  NotificationPollSource m_notificationPollSource{m_notificationManager};
  DeferredCallPollSource m_deferredCallPollSource;
  ProcessPollSource m_processPollSource{process::AsyncProcessManager::instance()};
  TimePollSource m_timePollSource{m_timeService};
  ConfigPollSource m_configPollSource{m_configService};
  DesktopEntryPollSource m_desktopEntryPollSource;
  IconThemePollSource m_iconThemePollSource;
  ClipboardPollSource m_clipboardPollSource{m_clipboardService};
  TimerPollSource m_timerPollSource;
  KeyRepeatPollSource m_keyRepeatPollSource{m_wayland};
  WorkspacePollSource m_workspacePollSource{m_compositorPlatform};
  KeyboardLayoutPollSource m_keyboardLayoutPollSource{m_compositorPlatform};
  LockKeysPollSource m_lockKeysPollSource{m_lockKeysService};
  std::unique_ptr<BrightnessPollSource> m_brightnessPollSource;
  std::unique_ptr<PipeWirePollSource> m_pipewirePollSource;
  std::unique_ptr<PipeWireSpectrumPollSource> m_pipewireSpectrumPollSource;
  std::unique_ptr<PolkitPollSource> m_polkitPollSource;
  IpcService m_ipcService;
  IpcPollSource m_ipcPollSource{m_ipcService};
  DmenuIpcService m_dmenuIpc;
  LocationService m_locationService;
  WeatherService m_weatherService;
  HttpClientPollSource m_httpClientPollSource{m_httpClient};
  FileWatchPollSource m_fileWatchPollSource{m_fileWatcher};
  LocationPollSource m_locationPollSource{m_locationService};
  WeatherPollSource m_weatherPollSource{m_weatherService};
  CalendarPollSource m_calendarPollSource{m_calendarService};
  CalendarReminderPollSource m_calendarReminderPollSource{m_calendarReminderMonitor};
  Timer m_polkitInitTimer;
  Timer m_polkitIdleCloseTimer;
  Timer m_greeterSyncTimeoutTimer;
  Timer m_greeterAutoSyncTimer;
  Timer m_clipboardAutoPasteTimer;
  Timer m_launcherAutoPasteTimer;
  Timer m_pluginAutoUpdateTimer;
  Timer m_graphicsRecoveryTimer;
  std::uint64_t m_greeterSyncGeneration = 0;
  int m_graphicsRecoveryAttempts = 0;
  bool m_graphicsRecoveryScheduled = false;

  std::unique_ptr<MainLoop> m_mainLoop;
};
