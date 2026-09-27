#include "config/managed_settings.h"

#include "config/generated_file.h"
#include "config/keybind_parse.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <vector>

namespace umbriel {

  namespace {

    constexpr std::string_view kHeader = "# Written by Umbriel when settings are changed from a settings app.\n"
                                         "# Loaded after config.toml and its includes, so these values win.\n\n";

    enum class Kind : uint8_t { Bool, Int, Real, Choice, Layout, Text, Action };

    constexpr std::array<std::string_view, 3> kDragToggle = {"none", "floating", "pinned"};
    constexpr std::array<std::string_view, 2> kTrackLayout = {"global", "window"};
    constexpr std::array<std::string_view, 2> kAccel = {"flat", "adaptive"};
    constexpr std::array<std::string_view, 2> kClickMethod = {"button_areas", "clickfinger"};
    constexpr std::array<std::string_view, 2> kTapMap = {"left_right_middle", "left_middle_right"};
    constexpr std::array<std::string_view, 5> kButtons = {
        "MouseLeft", "MouseRight", "MouseMiddle", "MouseBack", "MouseForward",
    };

    std::string text(bool value) { return value ? "true" : "false"; }
    std::string text(int value) { return std::to_string(value); }
    std::string text(double value) { return std::format("{:g}", value); }
    template <typename T> std::string text(const std::optional<T>& value) { return value ? text(*value) : ""; }

    std::string text(const std::optional<AccelProfile>& profile) {
      if (!profile) {
        return "";
      }
      switch (profile->kind) {
      case AccelProfile::Kind::Flat:
        return "flat";
      case AccelProfile::Kind::Adaptive:
        return "adaptive";
      case AccelProfile::Kind::Custom:
        return "custom";
      }
      return "";
    }

    std::string text(const std::optional<ClickMethod>& method) {
      if (!method) {
        return "";
      }
      return *method == ClickMethod::ButtonAreas ? "button_areas" : "clickfinger";
    }

    std::string text(const std::optional<TapButtonMap>& map) {
      if (!map) {
        return "";
      }
      return *map == TapButtonMap::LeftRightMiddle ? "left_right_middle" : "left_middle_right";
    }

    constexpr std::array<std::string_view, 5> kWindowsInStyles = {"popin", "zoom", "slide", "fade", "none"};
    constexpr std::array<std::string_view, 4> kWindowsOutStyles = {"fade", "slide", "popin", "zoom"};
    constexpr std::array<std::string_view, 3> kLayoutModes = {"scrolling", "dwindle", "master"};
    constexpr std::array<std::string_view, 3> kCenterFocused = {"never", "always", "on_overflow"};
    constexpr std::array<std::string_view, 3> kMasterPositions = {"left", "right", "center"};
    constexpr std::array<std::string_view, 4> kModKeys = {"Super", "Alt", "Ctrl", "Shift"};

    // Enumerators and choice arrays share their order.
    std::string text(LayoutMode mode) { return std::string(kLayoutModes[static_cast<size_t>(mode)]); }
    std::string text(CenterFocusedColumn center) { return std::string(kCenterFocused[static_cast<size_t>(center)]); }
    // Unset keeps the runtime default (Super on hardware, Alt nested), which has no fixed text.
    std::string text(const std::optional<ModifierKey>& key) {
      return key ? std::string(kModKeys[static_cast<size_t>(*key)]) : "";
    }
    std::string text(MasterPosition position) { return std::string(kMasterPositions[static_cast<size_t>(position)]); }

    std::string text(const std::optional<Keybind>& action) { return action ? formatAction(*action) : ""; }

    std::string text(WindowDragToggle toggle) { return std::string(kDragToggle[static_cast<size_t>(toggle)]); }
    std::string text(TrackLayout track) { return track == TrackLayout::Global ? "global" : "window"; }

    std::string scrollFactorText(const std::optional<Config::Input::Touchpad::ScrollFactor>& factor) {
      if (!factor) {
        return "";
      }
      return text(factor->horizontal ? factor->horizontal : factor->vertical);
    }

    std::string buttonText(const std::optional<uint32_t>& button) {
      const char* name = button ? mouseButtonName(*button) : nullptr;
      return name != nullptr ? name : "";
    }

    struct Spec {
      std::string_view key;
      Kind kind;
      double min = 0.0;
      double max = 0.0;
      std::span<const std::string_view> choices{};
      std::string (*get)(const Config&) = nullptr;
    };

    // Every entry maps to a key the config reader accepts; the ranges repeat the reader's own.
    const std::array kSpecs = {
        Spec{
            "input.middle_click_paste",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.middleClickPaste); }
        },
        Spec{
            "input.window_drag_toggle", Kind::Choice, 0, 0, kDragToggle,
            [](const Config& c) { return text(c.input.windowDragToggle); }
        },
        Spec{"input.keyboard.layout", Kind::Layout, 0, 0, {}, [](const Config& c) { return c.input.keyboard.layout; }},
        Spec{
            "input.keyboard.variant", Kind::Layout, 0, 0, {}, [](const Config& c) { return c.input.keyboard.variant; }
        },
        Spec{
            "input.keyboard.options", Kind::Layout, 0, 0, {}, [](const Config& c) { return c.input.keyboard.options; }
        },
        Spec{
            "input.keyboard.repeat_rate",
            Kind::Int,
            0,
            1000,
            {},
            [](const Config& c) { return text(c.input.keyboard.repeatRate); }
        },
        Spec{
            "input.keyboard.repeat_delay",
            Kind::Int,
            0,
            10000,
            {},
            [](const Config& c) { return text(c.input.keyboard.repeatDelay); }
        },
        Spec{
            "input.keyboard.numlock_toggle",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.keyboard.numlockToggle); }
        },
        Spec{
            "input.keyboard.track_layout", Kind::Choice, 0, 0, kTrackLayout,
            [](const Config& c) { return text(c.input.keyboard.trackLayout); }
        },
        Spec{"input.touchpad.tap", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.input.touchpad.tap); }},
        Spec{
            "input.touchpad.natural_scroll",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.touchpad.naturalScroll); }
        },
        Spec{
            "input.touchpad.natural_swipe",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.touchpad.naturalSwipe); }
        },
        Spec{
            "input.touchpad.left_handed",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.touchpad.leftHanded); }
        },
        Spec{
            "input.touchpad.accel_profile", Kind::Choice, 0, 0, kAccel,
            [](const Config& c) { return text(c.input.touchpad.accelProfile); }
        },
        Spec{
            "input.touchpad.sensitivity",
            Kind::Real,
            -1.0,
            1.0,
            {},
            [](const Config& c) { return text(c.input.touchpad.sensitivity); }
        },
        Spec{
            "input.touchpad.scroll_factor",
            Kind::Real,
            0.1,
            10.0,
            {},
            [](const Config& c) { return scrollFactorText(c.input.touchpad.scrollFactor); }
        },
        Spec{
            "input.touchpad.disable_while_typing",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.touchpad.disableWhileTyping); }
        },
        Spec{
            "input.touchpad.disable_on_external_mouse",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.touchpad.disableOnExternalMouse); }
        },
        Spec{
            "input.touchpad.click_method", Kind::Choice, 0, 0, kClickMethod,
            [](const Config& c) { return text(c.input.touchpad.clickMethod); }
        },
        Spec{
            "input.touchpad.tap_button_map", Kind::Choice, 0, 0, kTapMap,
            [](const Config& c) { return text(c.input.touchpad.tapButtonMap); }
        },
        Spec{
            "input.mouse.natural_scroll",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.mouse.naturalScroll); }
        },
        Spec{
            "input.mouse.left_handed",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.mouse.leftHanded); }
        },
        Spec{
            "input.mouse.accel_profile", Kind::Choice, 0, 0, kAccel,
            [](const Config& c) { return text(c.input.mouse.accelProfile); }
        },
        Spec{
            "input.mouse.sensitivity",
            Kind::Real,
            -1.0,
            1.0,
            {},
            [](const Config& c) { return text(c.input.mouse.sensitivity); }
        },
        Spec{
            "input.mouse.scroll_button", Kind::Choice, 0, 0, kButtons,
            [](const Config& c) { return buttonText(c.input.mouse.scrollButton); }
        },
        Spec{
            "input.mouse.scroll_button_lock",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.mouse.scrollButtonLock); }
        },
        Spec{"input.cursor.theme", Kind::Text, 0, 0, {}, [](const Config& c) { return c.input.cursor.theme; }},
        Spec{"input.cursor.size", Kind::Int, 1, 512, {}, [](const Config& c) { return text(c.input.cursor.size); }},
        Spec{
            "input.cursor.hide_when_typing",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.cursor.hideWhenTyping); }
        },
        Spec{
            "input.cursor.hide_timeout_ms",
            Kind::Int,
            0,
            3600000,
            {},
            [](const Config& c) { return text(c.input.cursor.hideTimeoutMs); }
        },
        Spec{
            "input.cursor.follows_focus",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.cursor.followsFocus); }
        },
        Spec{
            "input.focus.follows_mouse",
            Kind::Bool,
            0,
            0,
            {},
            [](const Config& c) { return text(c.input.focus.followsMouse); }
        },
        Spec{
            "input.focus.follows_mouse_max_scroll",
            Kind::Real,
            0.0,
            100.0,
            {},
            [](const Config& c) { return text(c.input.focus.followsMouseMaxScroll); }
        },
        Spec{
            "input.tablet.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.input.tablet.enabled); }
        },
        Spec{
            "input.tablet.map_to_output",
            Kind::Text,
            0,
            0,
            {},
            [](const Config& c) { return c.input.tablet.mapToOutput; }
        },
        Spec{"input.tablet.left_handed", Kind::Bool, 0, 0, {}, [](const Config& c) {
               return text(c.input.tablet.leftHanded);
             }},
        Spec{"appearance.border_width", Kind::Int, 0, 100, {}, [](const Config& c) { return text(c.appearance.borderWidth); }},
        Spec{"appearance.outer_border_width", Kind::Int, 0, 100, {}, [](const Config& c) { return text(c.appearance.outerBorderWidth); }},
        Spec{"appearance.corner_radius", Kind::Int, 0, 100, {}, [](const Config& c) { return text(c.appearance.cornerRadius); }},
        Spec{"appearance.drag_opacity", Kind::Real, 0.0, 1.0, {}, [](const Config& c) { return text(c.appearance.dragOpacity); }},
        Spec{"appearance.prefer_no_csd", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.appearance.preferNoCsd); }},
        Spec{"appearance.opaque_fullscreen", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.appearance.opaqueFullscreen); }},
        Spec{"appearance.blur.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.appearance.blur.enabled); }},
        Spec{"appearance.blur.optimized", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.appearance.blur.optimized); }},
        Spec{"appearance.blur.passes", Kind::Int, 0, 8, {}, [](const Config& c) { return text(c.appearance.blur.passes); }},
        Spec{"appearance.blur.radius", Kind::Int, 0, 100, {}, [](const Config& c) { return text(c.appearance.blur.radius); }},
        Spec{"appearance.blur.noise", Kind::Real, 0.0, 1.0, {}, [](const Config& c) { return text(c.appearance.blur.noise); }},
        Spec{"appearance.blur.brightness", Kind::Real, 0.0, 2.0, {}, [](const Config& c) { return text(c.appearance.blur.brightness); }},
        Spec{"appearance.blur.contrast", Kind::Real, 0.0, 2.0, {}, [](const Config& c) { return text(c.appearance.blur.contrast); }},
        Spec{"appearance.blur.saturation", Kind::Real, 0.0, 2.0, {}, [](const Config& c) { return text(c.appearance.blur.saturation); }},
        Spec{"appearance.shadow.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.appearance.shadow.enabled); }},
        Spec{"appearance.shadow.softness", Kind::Int, 0, 200, {}, [](const Config& c) { return text(c.appearance.shadow.softness); }},
        Spec{"appearance.shadow.offset_x", Kind::Int, -200, 200, {}, [](const Config& c) { return text(c.appearance.shadow.offsetX); }},
        Spec{"appearance.shadow.offset_y", Kind::Int, -200, 200, {}, [](const Config& c) { return text(c.appearance.shadow.offsetY); }},
        Spec{"animation.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.enabled); }},
        Spec{"animation.duration_ms", Kind::Int, 1, 10000, {}, [](const Config& c) { return text(c.animation.durationMs); }},
        Spec{"animation.windows_in.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.windowsIn.enabled); }},
        Spec{"animation.windows_out.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.windowsOut.enabled); }},
        Spec{"animation.windows_move.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.windowsMove.enabled); }},
        Spec{"animation.workspaces.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.workspaces.enabled); }},
        Spec{"animation.overview.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.overview.enabled); }},
        Spec{"animation.scratchpad.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.scratchpad.enabled); }},
        Spec{"animation.border.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.border.enabled); }},
        Spec{"animation.dim_unfocused.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.dimUnfocused.enabled); }},
        Spec{"animation.layers.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.animation.layers.enabled); }},
        Spec{"animation.windows_in.style", Kind::Choice, 0, 0, kWindowsInStyles, [](const Config& c) { return c.animation.windowsIn.style; }},
        Spec{"animation.windows_out.style", Kind::Choice, 0, 0, kWindowsOutStyles, [](const Config& c) { return c.animation.windowsOut.style; }},
        Spec{"animation.dim_unfocused.dim", Kind::Real, 0.0, 1.0, {}, [](const Config& c) { return text(c.animation.dimUnfocused.dim); }},
        Spec{"layout.mode", Kind::Choice, 0, 0, kLayoutModes, [](const Config& c) { return text(c.layout.mode); }},
        Spec{"layout.gap", Kind::Int, 0, 500, {}, [](const Config& c) { return text(c.layout.gap); }},
        Spec{"layout.scrolling.center_focused", Kind::Choice, 0, 0, kCenterFocused, [](const Config& c) { return text(c.layout.scrolling.centerFocused); }},
        Spec{"layout.scrolling.center_underfull_strip", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.layout.scrolling.centerUnderfullStrip); }},
        Spec{"layout.dwindle.preserve_split", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.layout.dwindle.preserveSplit); }},
        Spec{"layout.master.position", Kind::Choice, 0, 0, kMasterPositions, [](const Config& c) { return text(c.layout.master.position); }},
        Spec{"layout.master.default_width_fraction", Kind::Real, 0.1, 0.9, {}, [](const Config& c) { return text(c.layout.master.defaultWidthFraction); }},
        Spec{"layout.master.new_on_top", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.layout.master.newOnTop); }},
        Spec{"layout.master.new_becomes_master", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.layout.master.newBecomesMaster); }},
        Spec{"workspaces.back_and_forth", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.workspaces.backAndForth); }},
        Spec{"workspaces.empty_above", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.workspaces.emptyAbove); }},
        Spec{"overview.zoom", Kind::Real, 0.1, 0.75, {}, [](const Config& c) { return text(c.overview.zoom); }},
        Spec{"overview.background_blur", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.overview.backgroundBlur); }},
        Spec{"overview.workspace_wallpaper", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.overview.workspaceWallpaper); }},
        Spec{"overview.shortcuts", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.overview.shortcuts); }},
        Spec{"general.focus_on_activate", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.general.focusOnActivate); }},
        Spec{"hot_corners.top_left.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[0].enabled); }},
        Spec{"hot_corners.top_left.delay_ms", Kind::Int, 0, 10000, {}, [](const Config& c) { return text(c.hotCorners.corners[0].delayMs); }},
        Spec{"hot_corners.top_left.action", Kind::Action, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[0].action); }},
        Spec{"hot_corners.top_right.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[1].enabled); }},
        Spec{"hot_corners.top_right.delay_ms", Kind::Int, 0, 10000, {}, [](const Config& c) { return text(c.hotCorners.corners[1].delayMs); }},
        Spec{"hot_corners.top_right.action", Kind::Action, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[1].action); }},
        Spec{"hot_corners.bottom_left.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[2].enabled); }},
        Spec{"hot_corners.bottom_left.delay_ms", Kind::Int, 0, 10000, {}, [](const Config& c) { return text(c.hotCorners.corners[2].delayMs); }},
        Spec{"hot_corners.bottom_left.action", Kind::Action, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[2].action); }},
        Spec{"hot_corners.bottom_right.enabled", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[3].enabled); }},
        Spec{"hot_corners.bottom_right.delay_ms", Kind::Int, 0, 10000, {}, [](const Config& c) { return text(c.hotCorners.corners[3].delayMs); }},
        Spec{"hot_corners.bottom_right.action", Kind::Action, 0, 0, {}, [](const Config& c) { return text(c.hotCorners.corners[3].action); }},
        Spec{"general.mod_key", Kind::Choice, 0, 0, kModKeys, [](const Config& c) { return text(c.general.modKey); }},
        Spec{"general.show_cheatsheet", Kind::Bool, 0, 0, {}, [](const Config& c) { return text(c.general.showCheatsheet); }},
    };

    const std::array kKeys = [] {
      std::array<std::string_view, kSpecs.size()> keys{};
      std::ranges::transform(kSpecs, keys.begin(), &Spec::key);
      return keys;
    }();

    const Spec* findSpec(std::string_view key) {
      const auto it = std::ranges::find(kSpecs, key, &Spec::key);
      return it != kSpecs.end() ? &*it : nullptr;
    }

    // XKB layout, variant, and option names: lowercase words joined by the separators XKB uses.
    bool isXkbList(std::string_view text) {
      return text.size() <= 256 && std::ranges::all_of(text, [](char c) {
               return (c >= 'a' && c <= 'z')
                   || (c >= '0' && c <= '9')
                   || c == '_'
                   || c == ','
                   || c == ':'
                   || c == '+'
                   || c == '-'
                   || c == '('
                   || c == ')';
             });
    }

    bool isPlainText(std::string_view text) {
      return text.size() <= 256
          && std::ranges::none_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
    }

    std::vector<std::string_view> splitKey(std::string_view key) {
      std::vector<std::string_view> parts;
      for (size_t start = 0;;) {
        const size_t dot = key.find('.', start);
        parts.push_back(key.substr(start, dot - start));
        if (dot == std::string_view::npos) {
          return parts;
        }
        start = dot + 1;
      }
    }

    void applySetting(toml::table& root, std::string_view key, const ManagedSettingValue& value) {
      const std::vector<std::string_view> parts = splitKey(key);
      toml::table* table = &root;
      for (size_t i = 0; i + 1 < parts.size(); ++i) {
        table = &generatedTable(*table, {parts[i]});
      }
      const std::string_view leaf = parts.back();
      std::visit(
          [&]<typename T>(const T& stored) {
            if constexpr (std::is_same_v<T, std::monostate>) {
              table->erase(leaf);
            } else {
              table->insert_or_assign(leaf, stored);
            }
          },
          value
      );
    }

  } // namespace

  std::span<const std::string_view> managedSettingKeys() { return kKeys; }

  std::string managedSettingValue(const Config& config, std::string_view key) {
    const Spec* spec = findSpec(key);
    return spec != nullptr ? spec->get(config) : std::string();
  }

  ManagedSettingParse parseManagedSetting(std::string_view key, std::string_view text) {
    const Spec* spec = findSpec(key);
    if (spec == nullptr) {
      return {.value = std::nullopt, .error = std::format("'{}' is not a setting a settings app can change", key)};
    }
    if (text.empty()) {
      return {.value = ManagedSettingValue{std::monostate{}}, .error = {}};
    }
    const auto invalid = [&](std::string_view expected) {
      return ManagedSettingParse{.value = std::nullopt, .error = std::format("{} expects {}", key, expected)};
    };
    switch (spec->kind) {
    case Kind::Bool:
      if (text == "true" || text == "false") {
        return {.value = ManagedSettingValue{text == "true"}, .error = {}};
      }
      return invalid("true or false");
    case Kind::Int: {
      int64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() || value < spec->min || value > spec->max) {
        return invalid(std::format("an integer from {} to {}", spec->min, spec->max));
      }
      return {.value = ManagedSettingValue{value}, .error = {}};
    }
    case Kind::Real: {
      double value = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() || value < spec->min || value > spec->max) {
        return invalid(std::format("a number from {:g} to {:g}", spec->min, spec->max));
      }
      return {.value = ManagedSettingValue{value}, .error = {}};
    }
    case Kind::Choice:
      if (std::ranges::find(spec->choices, text) != spec->choices.end()) {
        return {.value = ManagedSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("one of the documented values");
    case Kind::Layout:
      if (isXkbList(text)) {
        return {.value = ManagedSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("XKB names");
    case Kind::Action: {
      Keybind parsed;
      if (isPlainText(text) && parseAction(text, parsed)) {
        return {.value = ManagedSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("an action a keybind accepts");
    }
    case Kind::Text:
      if (isPlainText(text)) {
        return {.value = ManagedSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("printable text");
    }
    return invalid("a valid value");
  }

  std::string editManagedSetting(std::string_view existing, std::string_view key, const ManagedSettingValue& value) {
    return editGeneratedToml(existing, kHeader, [&](toml::table& root) { applySetting(root, key, value); });
  }

  bool saveManagedSetting(const std::filesystem::path& file, std::string_view key, const ManagedSettingValue& value) {
    return rewriteGeneratedToml(file, kHeader, [&](toml::table& root) { applySetting(root, key, value); });
  }

  std::filesystem::path managedSettingsFile(const std::filesystem::path& configRoot) {
    return configRoot.parent_path() / "settings.toml";
  }

  bool saveManagedKeybind(const std::filesystem::path& file, std::string_view chord, std::string_view action) {
    return rewriteGeneratedToml(file, kHeader, [&](toml::table& root) {
      toml::table& keybinds = generatedTable(root, {"keybinds"});
      if (action.empty()) {
        keybinds.erase(chord);
      } else {
        keybinds.insert_or_assign(chord, std::string(action));
      }
      if (keybinds.empty()) {
        root.erase("keybinds");
      }
    });
  }

  std::vector<std::pair<std::string, std::string>> documentKeybinds(std::string_view document) {
    std::vector<std::pair<std::string, std::string>> keybinds;
    try {
      const toml::table table = toml::parse(document);
      const toml::table* section = table["keybinds"].as_table();
      if (section == nullptr) {
        return keybinds;
      }
      for (const auto& [chord, entry] : *section) {
        const toml::node* action = entry.is_table() ? entry.as_table()->get("action") : &entry;
        if (const auto text = action != nullptr ? action->value<std::string>() : std::nullopt) {
          keybinds.emplace_back(std::string(chord.str()), *text);
        }
      }
    } catch (const toml::parse_error&) {
      return {};
    }
    return keybinds;
  }

  bool documentSetsKey(std::string_view root, std::string_view key) {
    try {
      const toml::table table = toml::parse(root);
      return table.at_path(key).node() != nullptr;
    } catch (const toml::parse_error&) {
      return false;
    }
  }

} // namespace umbriel
