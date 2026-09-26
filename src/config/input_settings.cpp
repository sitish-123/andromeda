#include "config/input_settings.h"

#include "config/generated_file.h"
#include "config/keybind_parse.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <vector>

namespace umbriel {

  namespace {

    constexpr std::string_view kHeader = "# Written by Umbriel when input settings are changed from a settings app.\n"
                                         "# Keys in the including config.toml override these.\n\n";

    enum class Kind : uint8_t { Bool, Int, Real, Choice, Layout, Text };

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

    void applyInputSetting(toml::table& root, std::string_view key, const InputSettingValue& value) {
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

  std::span<const std::string_view> inputSettingKeys() { return kKeys; }

  std::string inputSettingValue(const Config& config, std::string_view key) {
    const Spec* spec = findSpec(key);
    return spec != nullptr ? spec->get(config) : std::string();
  }

  InputSettingParse parseInputSetting(std::string_view key, std::string_view text) {
    const Spec* spec = findSpec(key);
    if (spec == nullptr) {
      return {.value = std::nullopt, .error = std::format("'{}' is not an input setting", key)};
    }
    if (text.empty()) {
      return {.value = InputSettingValue{std::monostate{}}, .error = {}};
    }
    const auto invalid = [&](std::string_view expected) {
      return InputSettingParse{.value = std::nullopt, .error = std::format("{} expects {}", key, expected)};
    };
    switch (spec->kind) {
    case Kind::Bool:
      if (text == "true" || text == "false") {
        return {.value = InputSettingValue{text == "true"}, .error = {}};
      }
      return invalid("true or false");
    case Kind::Int: {
      int64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() || value < spec->min || value > spec->max) {
        return invalid(std::format("an integer from {} to {}", spec->min, spec->max));
      }
      return {.value = InputSettingValue{value}, .error = {}};
    }
    case Kind::Real: {
      double value = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() || value < spec->min || value > spec->max) {
        return invalid(std::format("a number from {:g} to {:g}", spec->min, spec->max));
      }
      return {.value = InputSettingValue{value}, .error = {}};
    }
    case Kind::Choice:
      if (std::ranges::find(spec->choices, text) != spec->choices.end()) {
        return {.value = InputSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("one of the documented values");
    case Kind::Layout:
      if (isXkbList(text)) {
        return {.value = InputSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("XKB names");
    case Kind::Text:
      if (isPlainText(text)) {
        return {.value = InputSettingValue{std::string(text)}, .error = {}};
      }
      return invalid("printable text");
    }
    return invalid("a valid value");
  }

  std::string editInputSetting(std::string_view existing, std::string_view key, const InputSettingValue& value) {
    return editGeneratedToml(existing, kHeader, [&](toml::table& root) { applyInputSetting(root, key, value); });
  }

  bool saveInputSetting(const std::filesystem::path& file, std::string_view key, const InputSettingValue& value) {
    return rewriteGeneratedToml(file, kHeader, [&](toml::table& root) { applyInputSetting(root, key, value); });
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
