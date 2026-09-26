#include "config/input_settings.h"

#include "check.h"
#include "config/store.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

using umbriel::documentSetsKey;
using umbriel::editInputSetting;
using umbriel::inputSettingKeys;
using umbriel::inputSettingValue;
using umbriel::InputSettingValue;
using umbriel::parseInputSetting;

namespace {
  bool accepts(std::string_view key, std::string_view text) { return parseInputSetting(key, text).value.has_value(); }

  // Values chosen to differ from every default, so a round trip proves the write took effect.
  constexpr std::array<std::string_view, 9> kSamples = {
      "false", "37", "0.5", "flat", "pinned", "window", "clickfinger", "left_middle_right", "MouseMiddle",
  };

  // The config reader compiles the keymap, so XKB keys need names XKB knows.
  std::string_view xkbSample(std::string_view key) {
    if (key == "input.keyboard.layout") {
      return "us,de";
    }
    if (key == "input.keyboard.variant") {
      return "dvorak";
    }
    return key == "input.keyboard.options" ? "caps:escape" : "";
  }
} // namespace

UMBRIEL_TEST(unknownKeysAreRejected) {
  CHECK(!accepts("input.touchpad.nope", "true"));
  CHECK(!accepts("general.autostart", "x"));
  CHECK(!parseInputSetting("input.nope", "").error.empty());
}

UMBRIEL_TEST(booleansAreStrict) {
  CHECK(accepts("input.touchpad.tap", "true"));
  CHECK(accepts("input.touchpad.tap", "false"));
  CHECK(!accepts("input.touchpad.tap", "yes"));
  CHECK(!accepts("input.touchpad.tap", "1"));
}

UMBRIEL_TEST(rangesIncludeTheirEdgesOnly) {
  CHECK(accepts("input.keyboard.repeat_rate", "0"));
  CHECK(accepts("input.keyboard.repeat_rate", "1000"));
  CHECK(!accepts("input.keyboard.repeat_rate", "1001"));
  CHECK(!accepts("input.keyboard.repeat_rate", "-1"));
  CHECK(accepts("input.touchpad.sensitivity", "-1"));
  CHECK(accepts("input.touchpad.sensitivity", "1.0"));
  CHECK(!accepts("input.touchpad.sensitivity", "1.01"));
}

UMBRIEL_TEST(malformedNumbersAreRejected) {
  CHECK(!accepts("input.touchpad.sensitivity", "0.3x"));
  CHECK(!accepts("input.keyboard.repeat_rate", "25.5"));
  CHECK(!accepts("input.keyboard.repeat_rate", " 25"));
}

UMBRIEL_TEST(choicesAreClosed) {
  CHECK(accepts("input.touchpad.accel_profile", "adaptive"));
  CHECK(!accepts("input.touchpad.accel_profile", "custom 0.2 0 1"));
  CHECK(!accepts("input.mouse.scroll_button", "MouseSide"));
}

UMBRIEL_TEST(layoutTextCannotBreakTheDocument) {
  CHECK(accepts("input.keyboard.options", "grp:alt_shift_toggle,caps:escape"));
  CHECK(accepts("input.keyboard.variant", "colemak_dh"));
  CHECK(!accepts("input.keyboard.layout", "us\"\n[general]"));
  CHECK(!accepts("input.keyboard.layout", "US"));
  CHECK(!accepts("input.cursor.theme", "Bibata\nx"));
}

UMBRIEL_TEST(emptyValueRemovesTheKey) {
  const auto parsed = parseInputSetting("input.touchpad.tap", "");
  CHECK(parsed.value.has_value() && std::holds_alternative<std::monostate>(*parsed.value));
  const std::string before = editInputSetting("", "input.touchpad.tap", InputSettingValue{false});
  CHECK(documentSetsKey(before, "input.touchpad.tap"));
  const std::string after = editInputSetting(before, "input.touchpad.tap", *parsed.value);
  CHECK(!documentSetsKey(after, "input.touchpad.tap"));
}

UMBRIEL_TEST(editsKeepUnrelatedKeys) {
  std::string doc =
      editInputSetting("[input.mouse]\nleft_handed = true\n", "input.touchpad.tap", InputSettingValue{false});
  doc = editInputSetting(doc, "input.keyboard.layout", InputSettingValue{std::string("us")});
  CHECK(documentSetsKey(doc, "input.mouse.left_handed"));
  CHECK(documentSetsKey(doc, "input.touchpad.tap"));
  CHECK(documentSetsKey(doc, "input.keyboard.layout"));
}

UMBRIEL_TEST(unsetOptionalsReadBackEmpty) {
  const umbriel::Config defaults;
  CHECK_EQ(inputSettingValue(defaults, "input.touchpad.natural_scroll"), std::string());
  CHECK_EQ(inputSettingValue(defaults, "input.touchpad.tap"), std::string("true"));
  CHECK_EQ(inputSettingValue(defaults, "input.nope"), std::string());
}

UMBRIEL_TEST(brokenDocumentsSetNothing) {
  CHECK(!documentSetsKey("[input.touchpad\ntap = ", "input.touchpad.tap"));
  CHECK(!documentSetsKey("", "input.touchpad.tap"));
}

UMBRIEL_TEST(everyKeyRoundTripsThroughTheConfigReader) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("umbriel-input-settings-" + std::to_string(getpid()) + ".toml");
  for (const std::string_view key : inputSettingKeys()) {
    std::string_view sample = xkbSample(key);
    InputSettingValue value;
    if (!sample.empty()) {
      value = *parseInputSetting(key, sample).value;
    }
    for (const std::string_view candidate : kSamples) {
      if (!sample.empty()) {
        break;
      }
      if (auto parsed = parseInputSetting(key, candidate); parsed.value) {
        sample = candidate;
        value = *parsed.value;
        break;
      }
    }
    CHECK(!sample.empty());
    std::ofstream(path) << editInputSetting("", key, value);
    umbriel::ConfigStore& store = umbriel::configStore();
    CHECK(store.load(path.c_str()));
    if (!store.diagnostics().empty()) {
      std::println(stderr, "{}: {}", key, store.diagnostics().front().message);
    }
    CHECK(store.diagnostics().empty());
    CHECK_EQ(inputSettingValue(store.config(), key), std::string(sample));
  }
  std::filesystem::remove(path);
}

int main() { return RUN_TESTS(); }
