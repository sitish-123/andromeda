#include "config/managed_settings.h"

#include "check.h"
#include "config/store.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>

using andromeda::documentSetsKey;
using andromeda::documentWorkspaceLayouts;
using andromeda::editManagedSetting;
using andromeda::managedSettingKeys;
using andromeda::managedSettingValue;
using andromeda::ManagedSettingValue;
using andromeda::parseManagedSetting;

namespace {
  bool accepts(std::string_view key, std::string_view text) { return parseManagedSetting(key, text).value.has_value(); }

  // Values chosen to differ from every default, so a round trip proves the write took effect.
  constexpr std::array<std::string_view, 23> kSamples = {
      "false",       "37",          "3",
      "0.5",         "flat",        "pinned",
      "window",      "clickfinger", "left_middle_right",
      "MouseMiddle", "dwindle",     "on_overflow",
      "right",       "zoom",        "overview-toggle",
      "Alt",         "light",       "#4a80ffff",
      "sticky",      "strip",       "hover",
      "macos",       "negotiated",
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

ANDROMEDA_TEST(unknownKeysAreRejected) {
  CHECK(!accepts("input.touchpad.nope", "true"));
  CHECK(!accepts("general.autostart", "x"));
  CHECK(!parseManagedSetting("input.nope", "").error.empty());
}

ANDROMEDA_TEST(booleansAreStrict) {
  CHECK(accepts("input.touchpad.tap", "true"));
  CHECK(accepts("input.touchpad.tap", "false"));
  CHECK(!accepts("input.touchpad.tap", "yes"));
  CHECK(!accepts("input.touchpad.tap", "1"));
}

ANDROMEDA_TEST(rangesIncludeTheirEdgesOnly) {
  CHECK(accepts("input.keyboard.repeat_rate", "0"));
  CHECK(accepts("input.keyboard.repeat_rate", "1000"));
  CHECK(!accepts("input.keyboard.repeat_rate", "1001"));
  CHECK(!accepts("input.keyboard.repeat_rate", "-1"));
  CHECK(accepts("input.touchpad.sensitivity", "-1"));
  CHECK(accepts("input.touchpad.sensitivity", "1.0"));
  CHECK(!accepts("input.touchpad.sensitivity", "1.01"));
}

ANDROMEDA_TEST(malformedNumbersAreRejected) {
  CHECK(!accepts("input.touchpad.sensitivity", "0.3x"));
  CHECK(!accepts("input.keyboard.repeat_rate", "25.5"));
  CHECK(!accepts("input.keyboard.repeat_rate", " 25"));
}

ANDROMEDA_TEST(choicesAreClosed) {
  CHECK(accepts("input.touchpad.accel_profile", "adaptive"));
  CHECK(!accepts("input.touchpad.accel_profile", "custom 0.2 0 1"));
  CHECK(!accepts("input.mouse.scroll_button", "MouseSide"));
}

ANDROMEDA_TEST(layoutTextCannotBreakTheDocument) {
  CHECK(accepts("input.keyboard.options", "grp:alt_shift_toggle,caps:escape"));
  CHECK(accepts("input.keyboard.variant", "colemak_dh"));
  CHECK(!accepts("input.keyboard.layout", "us\"\n[general]"));
  CHECK(!accepts("input.keyboard.layout", "US"));
  CHECK(!accepts("input.cursor.theme", "Bibata\nx"));
}

ANDROMEDA_TEST(emptyValueRemovesTheKey) {
  const auto parsed = parseManagedSetting("input.touchpad.tap", "");
  CHECK(parsed.value.has_value() && std::holds_alternative<std::monostate>(*parsed.value));
  const std::string before = editManagedSetting("", "input.touchpad.tap", ManagedSettingValue{false});
  CHECK(documentSetsKey(before, "input.touchpad.tap"));
  const std::string after = editManagedSetting(before, "input.touchpad.tap", *parsed.value);
  CHECK(!documentSetsKey(after, "input.touchpad.tap"));
}

ANDROMEDA_TEST(editsKeepUnrelatedKeys) {
  std::string doc =
      editManagedSetting("[input.mouse]\nleft_handed = true\n", "input.touchpad.tap", ManagedSettingValue{false});
  doc = editManagedSetting(doc, "input.keyboard.layout", ManagedSettingValue{std::string("us")});
  CHECK(documentSetsKey(doc, "input.mouse.left_handed"));
  CHECK(documentSetsKey(doc, "input.touchpad.tap"));
  CHECK(documentSetsKey(doc, "input.keyboard.layout"));
}

ANDROMEDA_TEST(unsetOptionalsReadBackEmpty) {
  const andromeda::Config defaults;
  CHECK_EQ(managedSettingValue(defaults, "input.touchpad.natural_scroll"), std::string());
  CHECK_EQ(managedSettingValue(defaults, "input.touchpad.tap"), std::string("true"));
  CHECK_EQ(managedSettingValue(defaults, "input.nope"), std::string());
}

ANDROMEDA_TEST(brokenDocumentsSetNothing) {
  CHECK(!documentSetsKey("[input.touchpad\ntap = ", "input.touchpad.tap"));
  CHECK(!documentSetsKey("", "input.touchpad.tap"));
}

ANDROMEDA_TEST(everyKeyRoundTripsThroughTheConfigReader) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("andromeda-managed-settings-" + std::to_string(getpid()) + ".toml");
  for (const std::string_view key : managedSettingKeys()) {
    std::string_view sample = xkbSample(key);
    ManagedSettingValue value;
    if (!sample.empty()) {
      value = *parseManagedSetting(key, sample).value;
    }
    for (const std::string_view candidate : kSamples) {
      if (!sample.empty()) {
        break;
      }
      if (auto parsed = parseManagedSetting(key, candidate); parsed.value) {
        sample = candidate;
        value = *parsed.value;
        break;
      }
    }
    CHECK(!sample.empty());
    std::ofstream(path) << editManagedSetting("", key, value);
    andromeda::ConfigStore& store = andromeda::configStore();
    store.load(path.c_str());
    if (!store.diagnostics().empty()) {
      std::println(stderr, "{}: {}", key, store.diagnostics().front().message);
    }
    CHECK(store.diagnostics().empty());
    CHECK_EQ(managedSettingValue(store.config(), key), std::string(sample));
  }
  std::filesystem::remove(path);
}

ANDROMEDA_TEST(volatileKeysStayOutOfTheFileSet) {
  using andromeda::isVolatileSetting;
  CHECK(isVolatileSetting("power.on_battery"));
  CHECK(!isVolatileSetting("input.touchpad.tap"));
  for (const std::string_view key : managedSettingKeys()) {
    CHECK(!isVolatileSetting(key));
  }
  const andromeda::Config defaults;
  CHECK_EQ(managedSettingValue(defaults, "power.on_battery"), std::string("false"));
  andromeda::setVolatileSetting("power.on_battery", ManagedSettingValue{true});
  CHECK_EQ(managedSettingValue(defaults, "power.on_battery"), std::string("true"));
  andromeda::setVolatileSetting("power.on_battery", ManagedSettingValue{std::monostate{}});
  CHECK_EQ(managedSettingValue(defaults, "power.on_battery"), std::string("false"));
}

// The live appearance keys govern the compositor's own settings: an accent recolors the focus accent, and reduce motion
// turns window opening into a fade and workspace slides into an in-place crossfade.
ANDROMEDA_TEST(liveAppearanceAppliesAccentAndReduceMotion) {
  andromeda::Config config;
  config.appearance.accent = std::array<float, 4>{1.0F, 0.0F, 0.5F, 1.0F};
  config.appearance.reduceMotion = true;
  andromeda::applyLiveAppearance(config);
  CHECK(config.colors.accentPrimary == (std::array<float, 4>{1.0F, 0.0F, 0.5F, 1.0F}));
  CHECK(config.animation.windowsIn.style == "fade");
  CHECK(config.animation.workspaces.enabled);
  CHECK(config.animation.workspaces.style == "reveal");
  const auto* crossfade = andromeda::findEffectPreset(config.effects, config.animation.workspaces.effect);
  CHECK(crossfade != nullptr && !crossfade->inert());
}

// animation.speed is one time scale for every event: twice the speed halves each duration and each spring's settle time.
ANDROMEDA_TEST(animationSpeedScalesResolvedDurations) {
  andromeda::Config config;
  config.animation.windowsMove.durationMs = 300;
  config.animation.overview.workspaceCurve = andromeda::AnimationCurve{.easing = andromeda::Easing::Spring};
  const int springBefore = andromeda::springDurationMs(config.animation.overview.workspaceCurve.spring);
  config.animation.speed = 2.0;
  andromeda::applyAnimationSpeed(config.animation);
  CHECK_EQ(config.animation.windowsMove.durationMs, 150);
  const int springAfter = andromeda::springDurationMs(config.animation.overview.workspaceCurve.spring);
  CHECK(std::abs(springAfter * 2 - springBefore) <= 2);
}

// Settings > Battery app rules: one [[window_rule]] per app in the settings file; a new choice replaces it, "normal"
// removes it, and a rule the user wrote by hand stays.
ANDROMEDA_TEST(appQosRulesReplaceAndRemoveOnlyTheirOwn) {
  const std::filesystem::path file =
      std::filesystem::temp_directory_path() / ("app-qos-" + std::to_string(getpid()) + ".toml");
  {
    std::ofstream out(file);
    out << "[[window_rule]]\nmatch.app_id = \"^foot$\"\nqos = \"never_throttle\"\nopacity = 0.9\n";
  }
  CHECK(andromeda::saveManagedAppQos(file, "org.mozilla.firefox", "never_throttle"));
  CHECK(andromeda::saveManagedAppQos(file, "org.mozilla.firefox", "freeze_when_hidden"));
  CHECK(andromeda::saveManagedAppQos(file, "Signal", "never_throttle"));
  const auto read = [&file] {
    std::ifstream in(file);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  };
  using Rules = std::vector<std::pair<std::string, std::string>>;
  CHECK(
      andromeda::documentAppQos(read())
      == (Rules{{"org.mozilla.firefox", "freeze_when_hidden"}, {"Signal", "never_throttle"}})
  );
  CHECK(andromeda::saveManagedAppQos(file, "org.mozilla.firefox", "normal"));
  CHECK(andromeda::documentAppQos(read()) == (Rules{{"Signal", "never_throttle"}}));
  CHECK(read().find("opacity = 0.9") != std::string::npos);
  std::filesystem::remove(file);
}

int main() { return RUN_TESTS(); }

ANDROMEDA_TEST(workspaceLayoutKeysWriteOneRulePerSlot) {
  CHECK(accepts("workspace.2.layout.mode", "master"));
  CHECK(!accepts("workspace.2.layout.mode", "grid"));
  CHECK(!accepts("workspace.0.layout.mode", "master"));
  const auto set = [](const std::string& doc, std::string_view key, std::string_view text) {
    return editManagedSetting(doc, key, *parseManagedSetting(key, text).value);
  };
  std::string doc = set("", "workspace.2.layout.mode", "master");
  doc = set(doc, "workspace.3.layout.mode", "floating");
  doc = set(doc, "workspace.2.layout.mode", "dwindle");
  using Rules = std::vector<std::pair<int64_t, std::string>>;
  CHECK((documentWorkspaceLayouts(doc) == Rules{{3, "floating"}, {2, "dwindle"}}));
  doc = set(doc, "workspace.3.layout.mode", "");
  CHECK((documentWorkspaceLayouts(doc) == Rules{{2, "dwindle"}}));
}
