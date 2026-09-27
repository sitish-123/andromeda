#include "check.h"
#include "output/mode_selection.h"

// Pulls the C++ math headers in before `static` is defined away below.
#include <cmath> // IWYU pragma: keep

extern "C" {
// wlroots uses C99 array parameter syntax in headers included by wlr_output.h.
#define static
#include <wlr/types/wlr_output.h>
#undef static
}

using umbriel::fastestPreferredMode;
using umbriel::outputCanAutoEnable;
using umbriel::OutputMode;
using umbriel::preferredFallbackMode;
using umbriel::selectOutputMode;

namespace {
  wlr_output_mode outputMode(int width, int height, int refresh, bool preferred = false) {
    return {
        .width = width,
        .height = height,
        .refresh = refresh,
        .preferred = preferred,
        .picture_aspect_ratio = WLR_OUTPUT_MODE_ASPECT_RATIO_NONE,
        .link = {},
    };
  }

  void addMode(wlr_output& output, wlr_output_mode& mode) { wl_list_insert(output.modes.prev, &mode.link); }
} // namespace

UMBRIEL_TEST(configuredResolutionSelectsClosestRefresh) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(5120, 1440, 119998, true);
  wlr_output_mode closest = outputMode(5120, 1440, 143987);
  addMode(output, lower);
  addMode(output, closest);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{5120, 1440, 144000});

  CHECK(selected == &closest);
  CHECK_EQ(selected->refresh, 143987);
}

UMBRIEL_TEST(unidentifiedOutputWithoutPreferredModeNeedsExplicitConfiguration) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode stale = outputMode(1920, 1080, 59994);
  addMode(output, stale);

  CHECK(!outputCanAutoEnable(&output));
}

UMBRIEL_TEST(preferredModeMakesUnconfiguredOutputSafeToEnable) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(1920, 1080, 60150, true);
  addMode(output, preferred);

  CHECK(outputCanAutoEnable(&output));
}

UMBRIEL_TEST(edidIdentityMakesUnconfiguredOutputSafeWithoutPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode mode = outputMode(1920, 1080, 60000);
  addMode(output, mode);
  char make[] = "Microstep";
  output.make = make;

  CHECK(outputCanAutoEnable(&output));
}

UMBRIEL_TEST(backendManagedOutputWithoutModesRemainsAutoEnabled) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(outputCanAutoEnable(&output));
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshPrefersMarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode fastest = outputMode(2560, 1440, 165000);
  wlr_output_mode preferred = outputMode(2560, 1440, 59951, true);
  addMode(output, fastest);
  addMode(output, preferred);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK(selected == &preferred);
  CHECK_EQ(selected->refresh, 59951);
}

UMBRIEL_TEST(configuredResolutionWithoutRefreshUsesHighestUnmarkedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode lower = outputMode(2560, 1440, 59951);
  wlr_output_mode highest = outputMode(2560, 1440, 119998);
  addMode(output, lower);
  addMode(output, highest);

  const wlr_output_mode* selected = selectOutputMode(&output, OutputMode{2560, 1440, 0});

  CHECK(selected == &highest);
  CHECK_EQ(selected->refresh, 119998);
}

// An unadvertised resolution stays a custom mode: the caller only leaves it behind once the commit fails.
UMBRIEL_TEST(unadvertisedResolutionSelectsNoAdvertisedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  addMode(output, preferred);

  CHECK(selectOutputMode(&output, OutputMode{5120, 1440, 143987}) == nullptr);
}

UMBRIEL_TEST(outputWithoutAdvertisedModesSelectsNoAdvertisedMode) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(selectOutputMode(&output, OutputMode{1280, 720, 0}) == nullptr);
}

UMBRIEL_TEST(failedCustomModeFallsBackToPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  wlr_output_mode slower = outputMode(2560, 1440, 59951);
  addMode(output, preferred);
  addMode(output, slower);

  const wlr_output_mode* fallback = preferredFallbackMode(&output, nullptr);

  CHECK(fallback == &preferred);
  CHECK_EQ(fallback->refresh, 119998);
}

UMBRIEL_TEST(failedAdvertisedModeFallsBackToPreferredMode) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  wlr_output_mode slower = outputMode(2560, 1440, 59951);
  addMode(output, preferred);
  addMode(output, slower);

  const wlr_output_mode* fallback = preferredFallbackMode(&output, &slower);

  CHECK(fallback == &preferred);
  CHECK_EQ(fallback->refresh, 119998);
}

// Retrying the mode that just failed would only fail again.
UMBRIEL_TEST(failedPreferredModeHasNoFallback) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode preferred = outputMode(2560, 1440, 119998, true);
  addMode(output, preferred);

  CHECK(preferredFallbackMode(&output, &preferred) == nullptr);
}

UMBRIEL_TEST(outputWithoutAdvertisedModesHasNoFallback) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(preferredFallbackMode(&output, nullptr) == nullptr);
}

// Ways the automatic mode can go wrong: a TV's preferred 4K@30 kept over its 4K@60, a faster mode at another
// resolution chosen, an output without a preferred mode left without one, and an output without modes given one.

UMBRIEL_TEST(automaticModeTakesHighestRefreshAtPreferredResolution) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode slow = outputMode(3840, 2160, 30000, true);
  wlr_output_mode fast = outputMode(3840, 2160, 60000);
  wlr_output_mode other = outputMode(1920, 1080, 120000);
  addMode(output, slow);
  addMode(output, fast);
  addMode(output, other);

  CHECK(fastestPreferredMode(&output) == &fast);
}

UMBRIEL_TEST(automaticModeKeepsPreferredWhenItIsFastest) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode slower = outputMode(2880, 1800, 60000);
  wlr_output_mode preferred = outputMode(2880, 1800, 120000, true);
  addMode(output, slower);
  addMode(output, preferred);

  CHECK(fastestPreferredMode(&output) == &preferred);
}

UMBRIEL_TEST(automaticModeWithoutPreferredUsesFirstModesResolution) {
  wlr_output output{};
  wl_list_init(&output.modes);
  wlr_output_mode first = outputMode(1920, 1080, 50000);
  wlr_output_mode faster = outputMode(1920, 1080, 60000);
  wlr_output_mode other = outputMode(1280, 720, 75000);
  addMode(output, first);
  addMode(output, faster);
  addMode(output, other);

  CHECK(fastestPreferredMode(&output) == &faster);
}

UMBRIEL_TEST(automaticModeIsNullWithoutModes) {
  wlr_output output{};
  wl_list_init(&output.modes);

  CHECK(fastestPreferredMode(&output) == nullptr);
}

int main() { return RUN_TESTS(); }
