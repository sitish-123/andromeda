#include "shell/settings/settings_registry_internal.h"
#include "config/app_icon_settings.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "pipewire/sound_player.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "ui/auth/clock_style.h"
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "ui/app_icon_colorization.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "compositors/settings_control.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  using namespace registry_detail;

  void registry_detail::add_security_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // Security
    for (const auto* key : {"offline_mode", "polkit_agent", "password_style"}) {
      appendRow(entries, shellRows, key);
    }
    const SettingVisibility lockscreenOn = [](const Config& c) { return c.lockscreen.enabled; };
    auto lockscreenRows =
        generateSchemaEntries(noctalia::config::schema::lockscreenSchema(), cfg.lockscreen, {"lockscreen"});
    appendRow(entries, lockscreenRows, "enabled");
    const auto lockRow = [&](std::string_view key, const SettingVisibility& visibleWhen) {
      appendRow(entries, lockscreenRows, key);
      entries.back().visibleWhen = visibleWhen;
    };
    for (const auto* key : {"lock_before_suspend", "grace_period_seconds", "fingerprint", "face"}) {
      lockRow(key, lockscreenOn);
    }
    lockRow("face_on_request", [](const Config& c) { return c.lockscreen.enabled && c.lockscreen.face; });
    lockRow("theme", lockscreenOn);
    {
      // Andromeda-only rows: Ryoku draws its own dial and ignores layout and clock options.
      const SettingVisibility andromedaOn = [](const Config& c) {
        return c.lockscreen.enabled && c.lockscreen.theme == "andromeda";
      };
      lockRow("layout", andromedaOn);
      std::vector<SelectOption> styles;
      for (const ClockStyle& style : clockStyles()) {
        styles.push_back(SelectOption{
            .value = std::string(style.id),
            .label = tr(std::format("settings.schema.lockscreen.clock-style.{}", style.id)),
        });
      }
      auto clockStyle = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.clock-style.label"),
          tr("settings.schema.lockscreen.clock-style.description"), {"lockscreen", "clock_style"},
          SelectSetting{std::move(styles), cfg.lockscreen.clockStyle}, "lock screen clock style font"
      );
      clockStyle.visibleWhen = andromedaOn;
      entries.push_back(std::move(clockStyle));
      for (const auto* key : {"clock_size", "clock_format_24h", "clock_seconds", "notification_content", "owner_message"}) {
        lockRow(key, andromedaOn);
      }
    }
    lockRow("allow_empty_password", lockscreenOn);
    if (env.screencopySupported) {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kLockscreenTransitions));
      for (const auto& opt : kLockscreenTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.lockscreen.transitions.size());
      for (const auto& transition : cfg.lockscreen.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kLockscreenTransitions, transition));
      }
      auto transitionEffects = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.transitions.label"),
          tr("settings.schema.lockscreen.transitions.description"), {"lockscreen", "transition"},
          std::move(transitions), "lock screen effects animation pool"
      );
      transitionEffects.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionEffects));
      lockRow("transition_duration", lockscreenOn);
      lockRow("edge_smoothness", lockscreenOn);
    }
    {
      const SettingVisibility lockscreenWallpaperOn = [](const Config& c) {
        return c.lockscreen.enabled;
      };
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.wallpaper.label"),
          tr("settings.schema.lockscreen.wallpaper.description"), {"lockscreen", "wallpaper"},
          TextSetting{
              .value = cfg.lockscreen.wallpaper,
              .placeholder = tr("settings.schema.lockscreen.wallpaper.placeholder"),
              .browseMode = TextSettingBrowseMode::OpenFile,
              .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
              .browseFallbackDirectory = wallpaper::resolveGlobalWallpaperDirectory(
                  cfg.wallpaper,
                  wallpaper::effectiveThemeMode(
                      shellThemeMode(cfg.theme), shellThemeMode(cfg.theme) == ThemeMode::Light
                  )
              ),
          },
          "lock screen background image custom"
      );
      e.visibleWhen = lockscreenWallpaperOn;
      entries.push_back(std::move(e));
    }
    lockRow("blur_intensity", lockscreenOn);
    lockRow("tint_intensity", lockscreenOn);
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.monitors.label"),
          tr("settings.schema.lockscreen.monitors.description"), {"lockscreen", "monitors"},
          ListSetting{.items = cfg.lockscreen.monitors, .suggestedOptions = env.availableOutputs},
          "lock screen monitor output connector"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Security, "lock-screen", tr("settings.schema.lockscreen.widgets.label"),
          tr("settings.schema.lockscreen.widgets.description"), {"lockscreen_widgets", "enabled"},
          ToggleSetting{cfg.lockscreenWidgets.enabled}, "lock screen widgets layout"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    auto privacyRows =
        generateSchemaEntries(noctalia::config::schema::privacySchema(), cfg.shell.privacy, {"shell", "privacy"});
    appendGroup(entries, privacyRows, "privacy");
    if (env.greeterSyncAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::Security, "greeter", tr("settings.schema.shell.greeter-sync-privilege-command.label"),
          tr("settings.schema.shell.greeter-sync-privilege-command.description"),
          {"shell", "greeter_sync", "privilege_command"},
          TextSetting{
              .value = cfg.shell.greeterSync.privilegeCommand,
              .placeholder = "pkexec",
              .browseFileExtensions = {},
          },
          "greeter sync pkexec run0 ghostty terminal sudo polkit wrapper"
      ));
    }
  }

  void registry_detail::add_shell_settings(RegistryContext& ctx) {
    using i18n::tr;
    const auto& cfg = ctx.cfg;
    const auto& env = ctx.env;
    auto& entries = ctx.entries;
    auto& shellRows = ctx.shellRows;

    // Shell
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = env.shellAvatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true)
        },
        "image picture"
    ));
    for (const auto* key : {"settings_window_translucent", "settings_expand_all_groups", "time_format", "date_format",
                            "readline_shortcuts"}) {
      appendRow(entries, shellRows, key);
    }
    entries.push_back(makeEntry(
        SettingsSection::Devices, "removable-media", tr("settings.schema.shell.automount-drives.label"),
        tr("settings.schema.shell.automount-drives.description"), {"shell", "automount_drives"},
        ToggleSetting{cfg.shell.automountDrives}, "usb drive disk stick card mount removable"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "keyboard-layout", tr("settings.schema.shell.keyboard-layout-custom-labels.label"),
        tr("settings.schema.shell.keyboard-layout-custom-labels.description"),
        {"shell", "keyboard_layout", "custom_labels"},
        StringMapSetting{
            .entries = cfg.shell.keyboardLayout.customLabels,
            .suggestedKeys = env.keyboardLayoutNames,
            .keyPlaceholder = tr("settings.widgets.map-placeholders.layout-name"),
            .valuePlaceholder = tr("settings.widgets.map-placeholders.value"),
        },
        "keyboard layout nickname alias label"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-as-systemd-services.label"),
        env.systemdUserManaged ? tr("settings.schema.shell.launch-apps-as-systemd-services.description")
                               : tr("settings.schema.shell.launch-apps-as-systemd-services.requires-systemd-session"),
        {"shell", "launch_apps_as_systemd_services"},
        ToggleSetting{
            .checked = cfg.shell.launchAppsAsSystemdServices,
            // Keep a leftover `true` switchable off even when the session cannot honor it.
            .enabled = env.systemdUserManaged || cfg.shell.launchAppsAsSystemdServices
        }
    ));
    {
      auto e = makeEntry(
          SettingsSection::Shell, "general", tr("settings.schema.shell.launch-apps-custom-command.label"),
          tr("settings.schema.shell.launch-apps-custom-command.description"), {"shell", "launch_apps_custom_command"},
          TextSetting{
              .value = cfg.shell.launchAppsCustomCommand,
              .placeholder = tr("settings.schema.shell.launch-apps-custom-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {},
          },
          "app command custom launcher dock taskbar"
      );
      e.visibleWhen = [managed = env.systemdUserManaged](const Config& c) {
        return !(c.shell.launchAppsAsSystemdServices && managed);
      };
      entries.push_back(std::move(e));
    }
    const SettingVisibility clipboardOn = [](const Config& c) { return c.shell.clipboardEnabled; };
    appendRow(entries, shellRows, "clipboard_enabled");
    // Deliberately not gated on clipboardOn: this keeps the live selection
    // alive, which is part of the transport that stays active when history
    // retention is turned off.
    appendRow(entries, shellRows, "clipboard_keep_from_closed_apps");
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-history-max-entries.label"),
          tr("settings.schema.shell.clipboard-history-max-entries.description"),
          {"shell", "clipboard_history_max_entries"},
          StepperSetting{
              .value = cfg.shell.clipboardHistoryMaxEntries,
              .minValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.min.value()),
              .maxValue = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.max.value()),
              .step = static_cast<int>(noctalia::config::schema::kClipboardHistoryMaxEntriesRange.step.value())
          },
          "clipboard history limit entries"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-confirm-clear-history.label"),
          tr("settings.schema.shell.clipboard-confirm-clear-history.description"),
          {"shell", "clipboard_confirm_clear_history"}, ToggleSetting{cfg.shell.clipboardConfirmClearHistory},
          "clipboard history clear confirm pinned"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-auto-paste.label"),
          tr("settings.schema.shell.clipboard-auto-paste.description"), {"shell", "clipboard_auto_paste"},
          enumSelect(kClipboardAutoPasteModes, cfg.shell.clipboardAutoPaste), "clipboard paste"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Shell, "clipboard", tr("settings.schema.shell.clipboard-image-action.label"),
          tr("settings.schema.shell.clipboard-image-action.description"), {"shell", "clipboard_image_action_command"},
          TextSetting{
              .value = cfg.shell.clipboardImageActionCommand,
              .placeholder = tr("settings.schema.shell.clipboard-image-action.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "clipboard image action annotation editor external gimp satty gradia"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    auto screenshotRows = generateSchemaEntries(
        noctalia::config::schema::screenshotSchema(), cfg.shell.screenshot, {"shell", "screenshot"}
    );
    for (const auto* key : {"freeze_screen", "confirm_region", "remember_last_region", "region_modifier", "show_cursor"}) {
      appendRow(entries, screenshotRows, key);
    }
    appendGroup(entries, screenshotRows, "screenshot-capture");
    appendGroup(entries, screenshotRows, "screenshot-annotation");
    for (const auto* key : {"copy_to_clipboard", "save_to_file", "directory", "filename_pattern", "format"}) {
      appendRow(entries, screenshotRows, key);
    }
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-quality.label"),
          tr("settings.schema.shell.screenshot-quality.description"), {"shell", "screenshot", "quality"},
          sliderFor(cfg.shell.screenshot.quality, noctalia::config::schema::kScreenshotQualityRange, true),
          "screenshot capture save file quality compression lossless jpeg webp jxl"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.format != ScreenshotFormat::Png; };
      entries.push_back(std::move(e));
    }
    appendRow(entries, screenshotRows, "pipe_to_command");
    {
      auto e = makeEntry(
          SettingsSection::Screenshot, "screenshot-output", tr("settings.schema.shell.screenshot-pipe-command.label"),
          tr("settings.schema.shell.screenshot-pipe-command.description"), {"shell", "screenshot", "pipe_command"},
          TextSetting{
              .value = cfg.shell.screenshot.pipeCommand,
              .placeholder = tr("settings.schema.shell.screenshot-pipe-command.placeholder"),
              .width = 320.0F,
              .browseFileExtensions = {}
          },
          "screenshot capture run pipe command stdin png"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.screenshot.pipeToCommand; };
      entries.push_back(std::move(e));
    }
    auto windowSwitcherRows = generateSchemaEntries(
        noctalia::config::schema::windowSwitcherSchema(), cfg.shell.windowSwitcher, {"shell", "window_switcher"}
    );
    appendGroup(entries, windowSwitcherRows, "window-switcher");
    // Keybinds
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.validate.label"),
        tr("settings.schema.keybinds.validate.description"), {"keybinds", "validate"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.validate, KeybindAction::Validate), .maxItems = 4
        },
        "keybind shortcut hotkey enter accept submit confirm"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.cancel.label"),
        tr("settings.schema.keybinds.cancel.description"), {"keybinds", "cancel"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.cancel, KeybindAction::Cancel), .maxItems = 4},
        "keybind shortcut hotkey escape close dismiss"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.left.label"),
        tr("settings.schema.keybinds.left.description"), {"keybinds", "left"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.left, KeybindAction::Left), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.right.label"),
        tr("settings.schema.keybinds.right.description"), {"keybinds", "right"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.right, KeybindAction::Right), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.up.label"),
        tr("settings.schema.keybinds.up.description"), {"keybinds", "up"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.up, KeybindAction::Up), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.down.label"),
        tr("settings.schema.keybinds.down.description"), {"keybinds", "down"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.down, KeybindAction::Down), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-previous.label"),
        tr("settings.schema.keybinds.tab-previous.description"), {"keybinds", "tab_previous"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.tabPrevious, KeybindAction::TabPrevious), .maxItems = 4
        },
        "keybind shortcut hotkey shift tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-next.label"),
        tr("settings.schema.keybinds.tab-next.description"), {"keybinds", "tab_next"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.tabNext, KeybindAction::TabNext), .maxItems = 4},
        "keybind shortcut hotkey tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.copy.label"),
        tr("settings.schema.keybinds.copy.description"), {"keybinds", "copy"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.copy, KeybindAction::Copy), .maxItems = 4},
        "keybind shortcut hotkey copy clipboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.save.label"),
        tr("settings.schema.keybinds.save.description"), {"keybinds", "save"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.save, KeybindAction::Save), .maxItems = 4},
        "keybind shortcut hotkey save file"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.delete.label"),
        tr("settings.schema.keybinds.delete.description"), {"keybinds", "delete"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.deleteEntry, KeybindAction::Delete), .maxItems = 4
        },
        "keybind shortcut hotkey delete remove clear"
    ));

    // Andromeda-specific integrations
    if (env.andromedaOverviewTypeToLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Services, "audio", tr("settings.schema.shell.andromeda-audio-provider.label"),
          tr("settings.schema.shell.andromeda-audio-provider.description"),
          {"shell", "andromeda_audio_provider_enabled"}, ToggleSetting{cfg.shell.andromedaAudioProviderEnabled},
          "andromeda audio spectrum shader effects provider"
      ));
    }
    if (env.andromedaOverviewCloseOnLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Overview, "overview", tr("settings.schema.shell.andromeda-overview-close-on-launch.label"),
          tr("settings.schema.shell.andromeda-overview-close-on-launch.description"),
          {"shell", "andromeda_overview_close_on_launch_enabled"},
          ToggleSetting{cfg.shell.andromedaOverviewCloseOnLaunchEnabled}, "andromeda overview close app launch launcher"
      ));
    }
  }

} // namespace settings
