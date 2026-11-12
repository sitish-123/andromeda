#pragma once

#include "config/schema/field.h"

// Per-section field schemas: the single source of truth for reading, writing,
// and validating each config section. Field order is the config_export::serialize
// emission order.
namespace noctalia::config::schema {

  const Schema<AudioConfig>& audioSchema();
  const Schema<WeatherConfig>& weatherSchema();
  const Schema<StorageConfig>& storageSchema();
  const Schema<OsdConfig>& osdSchema();
  const Schema<LockscreenConfig>& lockscreenSchema();
  const Schema<SystemConfig>& systemSchema();
  const Schema<NightLightConfig>& nightlightSchema();
  const Schema<LocationConfig>& locationSchema();
  const Schema<NotificationConfig>& notificationSchema();
  const Schema<DockConfig>& dockSchema();
  const Schema<DesktopIconsConfig>& desktopIconsSchema();
  const Schema<DesktopWidgetsConfig>& desktopWidgetsSchema();
  const Schema<LockscreenWidgetsConfig>& lockscreenWidgetsSchema();
  const Schema<BrightnessConfig>& brightnessSchema();
  const Schema<BatteryConfig>& batterySchema();
  const Schema<BluetoothConfig>& bluetoothSchema();
  const Schema<ControlCenterConfig>& controlCenterSchema();
  const Schema<ShellConfig::ScreenshotConfig>& screenshotSchema(); // [shell.screenshot]
  const Schema<ShellConfig::LauncherConfig>& launcherSchema();     // [shell.launcher]
  const Schema<ShellConfig::PanelConfig>& panelSchema();           // [shell.panel]
  const Schema<ShellConfig::WindowSwitcherConfig>& windowSwitcherSchema(); // [shell.window_switcher]
  const Schema<ShellConfig::PrivacyConfig>& privacySchema();               // [shell.privacy]
  const Schema<ShellConfig::AnimationConfig>& animationSchema();           // [shell.animation]
  const Schema<ShellConfig::ScreenCornersConfig>& screenCornersSchema();   // [shell.screen_corners]
  const Schema<ShellSessionConfig>& sessionSchema();                       // [shell.session]
  const Schema<SystemConfig::MonitorConfig>& systemMonitorRowsSchema();    // [system.monitor]
  const Schema<ControlCenterConfig::CalendarTabConfig>& calendarTabRowsSchema(); // [control_center.calendar]
  const Schema<WallpaperAutomationConfig>& wallpaperAutomationRowsSchema(); // [wallpaper.automation]
  const Schema<PluginsConfig>& pluginsSchema();
  const Schema<HotCornersConfig>& hotCornersSchema();
  const Schema<CalendarConfig::Reminders>& calendarRemindersSchema();
  const Schema<CalendarConfig>& calendarSchema();
  const Schema<KeybindsConfig>& keybindsSchema();
  const Schema<HooksConfig>& hooksSchema();
  const Schema<IdleConfig>& idleSchema();
  const Schema<WallpaperConfig>& wallpaperSchema();
  const Schema<ThemeConfig>& themeSchema();
  const Schema<ShellConfig>& shellSchema();
  const Schema<AccessibilityConfig>& accessibilitySchema();

  // Bar is handled at the config root (named [bar.<name>] tables + an `order`
  // array live on Config::bars directly, not in a section struct), so its
  // schemas are consumed by hand-written loops in config_service/config_export
  // rather than a single section writeTable.
  //
  //  - barFieldsSchema: every concrete BarConfig field EXCEPT position/name.
  //    Used for the base bar (read + write) and the resolve-and-flatten write of
  //    each monitor override.
  //  - barMonitorOverrideSchema: the parallel optional fields of a monitor
  //    override, used for READ only (overrides serialize via barFieldsSchema on
  //    the resolved bar).
  const Schema<BarConfig>& barFieldsSchema();
  const Schema<BarMonitorOverride>& barMonitorOverrideSchema();
  const Schema<BarDeadZoneConfig>& barDeadZoneSchema();

  // True if `path` (a `{section, key, ...}` override path, e.g. {"shell","ui_scale"}
  // or {"bar","default","monitor","DP-1","thickness"}) resolves to a real key in the
  // schema. Lets the settings UI verify its hand-written override paths against the
  // single schema source instead of drifting silently. Pure schema resolution — no
  // ConfigService dependency.
  [[nodiscard]] bool isKnownConfigPath(const std::vector<std::string>& path);

} // namespace noctalia::config::schema
