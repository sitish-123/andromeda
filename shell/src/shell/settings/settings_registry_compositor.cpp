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
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
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

  namespace registry_detail {

    constexpr std::string_view kCompositorPathRoot = "andromeda";

    std::vector<std::string> compositorPath(std::string_view key) {
      std::vector<std::string> path{std::string(kCompositorPathRoot)};
      for (const auto part : std::views::split(key, '.')) {
        path.emplace_back(std::string_view(part));
      }
      return path;
    }

    // "appearance.blur.radius" -> "settings.schema.andromeda.appearance-blur-radius"
    std::string compositorLabelKey(std::string_view key) {
      std::string id(key);
      std::ranges::replace(id, '.', '-');
      std::ranges::replace(id, '_', '-');
      return "settings.schema.andromeda." + id;
    }

    double compositorNumber(const SettingsControl& compositor, std::string_view key) {
      const std::string text = compositor.value(key);
      double value = 0.0;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    // Compositor settings, shown only once the compositor's settings manager has reported them. Options that only
    // apply in one mode (a layout's own options, blur strength with blur off) are left out until they apply; the
    // registry rebuilds whenever the compositor reports a change.
    void addCompositorEntries(std::vector<SettingEntry>& entries, const SettingsControl& compositor) {
      using i18n::tr;
      const auto isOn = [&](std::string_view key) { return compositor.value(key) == "true"; };
      const auto add = [&](SettingsSection section, std::string_view group, std::string_view key,
                           SettingControl control, std::string tags, bool advanced = false) {
        const std::string labelKey = compositorLabelKey(key);
        entries.push_back(makeEntry(
            section, std::string(group), tr(labelKey + ".label"), tr(labelKey + ".description"), compositorPath(key),
            std::move(control), std::move(tags), advanced
        ));
      };
      const auto toggle = [&](SettingsSection section, std::string_view group, std::string_view key, std::string tags,
                              bool advanced = false) {
        add(section, group, key, ToggleSetting{isOn(key)}, std::move(tags), advanced);
      };
      const auto slider = [&](SettingsSection section, std::string_view group, std::string_view key, double min,
                              double max, double step, bool integer, std::string tags, bool advanced = false) {
        add(section, group, key, SliderSetting(compositorNumber(compositor, key), min, max, step, integer),
            std::move(tags), advanced);
      };
      const auto stepper = [&](SettingsSection section, std::string_view group, std::string_view key, int min, int max,
                               int step, std::string suffix, std::string tags, bool advanced = false) {
        add(section, group, key,
            StepperSetting{
                .value = static_cast<int>(compositorNumber(compositor, key)),
                .minValue = min,
                .maxValue = max,
                .step = step,
                .valueSuffix = std::move(suffix),
            },
            std::move(tags), advanced);
      };
      const auto choice = [&](SettingsSection section, std::string_view group, std::string_view key,
                              std::initializer_list<std::string_view> values, bool segmented, std::string tags) {
        const std::string labelKey = compositorLabelKey(key);
        std::vector<SelectOption> options;
        for (const std::string_view value : values) {
          std::string optionId(value);
          std::ranges::replace(optionId, '_', '-');
          options.push_back(SelectOption{std::string(value), tr(labelKey + ".options." + optionId)});
        }
        SelectSetting select{std::move(options), compositor.value(key)};
        select.segmented = segmented;
        add(section, group, key, std::move(select), std::move(tags));
      };

      using enum SettingsSection;
      stepper(WindowStyle, "window-borders", "appearance.border_width", 0, 100, 1, "px", "window border width outline");
      stepper(WindowStyle, "window-borders", "appearance.corner_radius", 0, 100, 1, "px", "window rounded corners");
      stepper(
          WindowStyle, "window-borders", "appearance.outer_border_width", 0, 100, 1, "px", "window outer border", true
      );
      toggle(WindowStyle, "window-shadow", "appearance.shadow.enabled", "window drop shadow");
      if (isOn("appearance.shadow.enabled")) {
        slider(WindowStyle, "window-shadow", "appearance.shadow.softness", 0, 200, 1, true, "window shadow blur size");
        stepper(
            WindowStyle, "window-shadow", "appearance.shadow.offset_x", -200, 200, 1, "px", "window shadow offset", true
        );
        stepper(
            WindowStyle, "window-shadow", "appearance.shadow.offset_y", -200, 200, 1, "px", "window shadow offset", true
        );
      }
      toggle(WindowStyle, "window-blur", "appearance.blur.enabled", "window background blur transparency");
      if (isOn("appearance.blur.enabled")) {
        slider(WindowStyle, "window-blur", "appearance.blur.radius", 0, 100, 1, true, "blur strength radius");
        slider(WindowStyle, "window-blur", "appearance.blur.passes", 0, 8, 1, true, "blur quality passes");
        toggle(WindowStyle, "window-blur", "appearance.blur.optimized", "blur optimized performance", true);
        slider(WindowStyle, "window-blur", "appearance.blur.noise", 0.0, 1.0, 0.01, false, "blur noise grain", true);
        slider(
            WindowStyle, "window-blur", "appearance.blur.brightness", 0.0, 2.0, 0.05, false, "blur brightness", true
        );
        slider(WindowStyle, "window-blur", "appearance.blur.contrast", 0.0, 2.0, 0.05, false, "blur contrast", true);
        slider(
            WindowStyle, "window-blur", "appearance.blur.saturation", 0.0, 2.0, 0.05, false, "blur saturation", true
        );
      }
      toggle(WindowStyle, "window-decorations", "appearance.prefer_no_csd", "client side decorations title bar csd");
      slider(
          WindowStyle, "window-decorations", "appearance.drag_opacity", 0.0, 1.0, 0.05, false,
          "window drag move opacity transparency"
      );
      toggle(WindowStyle, "window-decorations", "appearance.opaque_fullscreen", "fullscreen opaque transparency", true);

      toggle(TextScale, "accessibility", "appearance.increase_contrast", "accessibility high contrast visually impaired");
      toggle(
          TextScale, "accessibility", "appearance.reduce_transparency",
          "accessibility reduce transparency glass solid opaque"
      );
      if (!isOn("appearance.reduce_transparency")) {
        slider(
            Appearance, "surfaces", "appearance.glass", 0.0, 1.0, 0.05, false,
            "glass clear tinted frosted liquid transparency material blur"
        );
      }
      toggle(Motion, "motion", "appearance.reduce_motion", "accessibility reduce motion animations");

      toggle(Motion, "window-motion", "animation.enabled", "window animations compositor");
      if (isOn("animation.enabled")) {
        stepper(Motion, "window-motion", "animation.duration_ms", 1, 10000, 25, "ms", "animation duration speed");
        toggle(Motion, "window-motion", "animation.windows_in.enabled", "window open animation");
        if (isOn("animation.windows_in.enabled")) {
          choice(
              Motion, "window-motion", "animation.windows_in.style", {"popin", "zoom", "slide", "fade", "none"}, false,
              "window open animation style"
          );
        }
        toggle(Motion, "window-motion", "animation.windows_out.enabled", "window close animation");
        if (isOn("animation.windows_out.enabled")) {
          choice(
              Motion, "window-motion", "animation.windows_out.style", {"fade", "slide", "popin", "zoom"}, false,
              "window close animation style"
          );
        }
        toggle(Motion, "window-motion", "animation.windows_move.enabled", "window move resize animation");
        toggle(Motion, "window-motion", "animation.workspaces.enabled", "workspace switch animation");
        toggle(Motion, "window-motion", "animation.overview.enabled", "overview animation");
        toggle(Motion, "window-motion", "animation.scratchpad.enabled", "scratchpad animation");
        toggle(Motion, "window-motion", "animation.border.enabled", "border focus color animation");
        toggle(Motion, "window-motion", "animation.layers.enabled", "panel layer surface animation", true);
      }
      toggle(Motion, "dim-unfocused", "animation.dim_unfocused.enabled", "dim inactive unfocused windows");
      if (isOn("animation.dim_unfocused.enabled")) {
        slider(Motion, "dim-unfocused", "animation.dim_unfocused.dim", 0.0, 1.0, 0.05, false, "dim amount");
      }

      const std::string mode = compositor.value("layout.mode");
      choice(Layout, "layout", "layout.mode", {"scrolling", "dwindle", "master"}, true, "tiling layout mode");
      stepper(Layout, "layout", "layout.gap", 0, 500, 1, "px", "gaps spacing between windows");
      if (mode == "scrolling") {
        choice(
            Layout, "layout", "layout.scrolling.center_focused", {"never", "always", "on_overflow"}, false,
            "center focused column"
        );
        toggle(Layout, "layout", "layout.scrolling.center_underfull_strip", "center columns");
      } else if (mode == "dwindle") {
        toggle(Layout, "layout", "layout.dwindle.preserve_split", "split direction");
      } else if (mode == "master") {
        choice(Layout, "layout", "layout.master.position", {"left", "right", "center"}, true, "master stack position");
        slider(Layout, "layout", "layout.master.default_width_fraction", 0.1, 0.9, 0.05, false, "master width ratio");
        toggle(Layout, "layout", "layout.master.new_on_top", "new window stack order");
        toggle(Layout, "layout", "layout.master.new_becomes_master", "new window master");
      }
      toggle(Layout, "workspaces", "workspaces.back_and_forth", "workspace back and forth previous");
      toggle(Layout, "workspaces", "workspaces.empty_above", "workspace empty");

      slider(Overview, "overview", "overview.zoom", 0.1, 0.75, 0.05, false, "overview zoom scale size");
      toggle(Overview, "overview", "overview.background_blur", "overview blur wallpaper");
      toggle(Overview, "overview", "overview.workspace_wallpaper", "overview wallpaper preview");
      toggle(Overview, "overview", "overview.shortcuts", "overview keyboard shortcut badges");

      toggle(Windows, "focus", "input.focus.follows_mouse", "focus follows mouse hover pointer");
      toggle(Windows, "focus", "input.cursor.follows_focus", "cursor warp follows focus");
      toggle(Windows, "focus", "general.focus_on_activate", "focus activation request urgent");
      toggle(Windows, "focus", "input.window_snapping", "snap tile half quarter edge drag");

      choice(Shortcuts, "modifier", "general.mod_key", {"Super", "Alt", "Ctrl", "Shift"}, true, "mod key modifier");

      // Hot corners run a compositor action; the shell's own panels are reached through shell: actions.
      constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kCornerActions{{
          {"shell:panel-toggle launcher", "launcher"},
          {"shell:panel-toggle control-center", "control-center"},
          {"shell:window-switcher", "window-switcher"},
          {"overview-toggle", "overview"},
      }};
      for (const std::string_view corner : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
        const std::string prefix = "hot_corners." + std::string(corner);
        toggle(Windows, "hot-corners", prefix + ".enabled", "hot corner screen edge pointer " + std::string(corner));
        if (!isOn(prefix + ".enabled")) {
          continue;
        }
        const std::string actionKey = prefix + ".action";
        const std::string current = compositor.value(actionKey);
        std::vector<SelectOption> options;
        for (const auto& [value, id] : kCornerActions) {
          options.push_back(
              SelectOption{std::string(value), tr("settings.schema.andromeda.hot-corner-actions." + std::string(id))}
          );
        }
        if (!current.empty()
            && std::ranges::none_of(kCornerActions, [&](const auto& a) { return a.first == current; })) {
          options.push_back(SelectOption{current, current});
        }
        add(Windows, "hot-corners", actionKey, SelectSetting{std::move(options), current}, "hot corner action");
        stepper(Windows, "hot-corners", prefix + ".delay_ms", 0, 10000, 50, "ms", "hot corner delay");
      }
    }

  } // namespace registry_detail

  std::optional<std::string> compositorSettingKey(const std::vector<std::string>& path) {
    if (path.size() < 2 || path.front() != kCompositorPathRoot) {
      return std::nullopt;
    }
    return StringUtils::join(std::vector<std::string>(path.begin() + 1, path.end()), ".");
  }

} // namespace settings
