#include "config/value_parse.h"

#include "check.h"

#include <array>
#include <cmath>

namespace {

  bool near(float actual, float expected) { return std::fabs(actual - expected) < 0.001F; }

} // namespace

ANDROMEDA_TEST(parsesSixDigitColorAsOpaque) {
  std::array<float, 4> color{};
  CHECK(andromeda::parseColor("#ff8000", color));
  CHECK(near(color[0], 1.0F));
  CHECK(near(color[1], 0.50196F));
  CHECK(near(color[2], 0.0F));
  CHECK(near(color[3], 1.0F));
}

ANDROMEDA_TEST(parsesEightDigitColorWithAlpha) {
  std::array<float, 4> color{};
  CHECK(andromeda::parseColor("#00000080", color));
  CHECK(near(color[0], 0.0F));
  CHECK(near(color[3], 0.50196F));
}

ANDROMEDA_TEST(colorHexIsCaseInsensitive) {
  std::array<float, 4> lower{};
  std::array<float, 4> upper{};
  CHECK(andromeda::parseColor("#abcdef", lower));
  CHECK(andromeda::parseColor("#ABCDEF", upper));
  CHECK(lower == upper);
}

ANDROMEDA_TEST(rejectsMalformedColors) {
  std::array<float, 4> color{0.25F, 0.25F, 0.25F, 0.25F};
  const std::array<float, 4> before = color;

  CHECK(!andromeda::parseColor("", color));
  CHECK(!andromeda::parseColor("#", color));
  CHECK(!andromeda::parseColor("ff8000", color));     // no leading '#'
  CHECK(!andromeda::parseColor("#fff", color));       // 3-digit shorthand is not supported
  CHECK(!andromeda::parseColor("#ffff", color));      // 4-digit shorthand is not supported
  CHECK(!andromeda::parseColor("#ff80", color));      // wrong length
  CHECK(!andromeda::parseColor("#ff800", color));     // wrong length
  CHECK(!andromeda::parseColor("#gg8000", color));    // non-hex digits
  CHECK(!andromeda::parseColor("#ff80000", color));   // wrong length
  CHECK(!andromeda::parseColor("#ff8000000", color)); // wrong length

  // A rejected parse must not disturb the caller's value.
  CHECK(color == before);
}

ANDROMEDA_TEST(parsesOutputModeWithoutRefresh) {
  andromeda::OutputMode mode{};
  CHECK(andromeda::parseOutputMode("1920x1080", mode));
  CHECK_EQ(mode.width, 1920);
  CHECK_EQ(mode.height, 1080);
  CHECK_EQ(mode.refreshMHz, 0);
}

ANDROMEDA_TEST(parsesOutputModeWithRefresh) {
  andromeda::OutputMode mode{};
  CHECK(andromeda::parseOutputMode("2560x1440@165", mode));
  CHECK_EQ(mode.width, 2560);
  CHECK_EQ(mode.height, 1440);
  CHECK_EQ(mode.refreshMHz, 165000);
}

ANDROMEDA_TEST(parsesFractionalRefresh) {
  andromeda::OutputMode mode{};
  CHECK(andromeda::parseOutputMode("1920x1080@59.951", mode));
  CHECK_EQ(mode.refreshMHz, 59951);
}

ANDROMEDA_TEST(clampsOutOfRangeOutputMode) {
  andromeda::OutputMode mode{};
  CHECK(andromeda::parseOutputMode("0x0", mode));
  CHECK_EQ(mode.width, 1);
  CHECK_EQ(mode.height, 1);

  CHECK(andromeda::parseOutputMode("99999x99999@5000", mode));
  CHECK_EQ(mode.width, 16384);
  CHECK_EQ(mode.height, 16384);
  CHECK_EQ(mode.refreshMHz, 1000000);
}

ANDROMEDA_TEST(rejectsMalformedOutputModes) {
  andromeda::OutputMode mode{};
  CHECK(!andromeda::parseOutputMode("", mode));
  CHECK(!andromeda::parseOutputMode("1920", mode));       // no separator
  CHECK(!andromeda::parseOutputMode("x1080", mode));      // empty width
  CHECK(!andromeda::parseOutputMode("1920x", mode));      // empty height
  CHECK(!andromeda::parseOutputMode("1920x1080@", mode)); // empty refresh
  CHECK(!andromeda::parseOutputMode("axb", mode));        // non-numeric
  CHECK(!andromeda::parseOutputMode("1920x1080@abc", mode));
  CHECK(!andromeda::parseOutputMode("1920x1080p", mode)); // trailing garbage
  CHECK(!andromeda::parseOutputMode("1920 x 1080", mode));
}

ANDROMEDA_TEST(validatesEnvironmentVariableNames) {
  CHECK(andromeda::isEnvironmentVariableName("DXVK_HDR"));
  CHECK(andromeda::isEnvironmentVariableName("_PRIVATE"));
  CHECK(andromeda::isEnvironmentVariableName("A1"));

  CHECK(!andromeda::isEnvironmentVariableName(""));
  CHECK(!andromeda::isEnvironmentVariableName("1STARTS_WITH_DIGIT"));
  CHECK(!andromeda::isEnvironmentVariableName("HAS-HYPHEN"));
  CHECK(!andromeda::isEnvironmentVariableName("HAS.DOT"));
  CHECK(!andromeda::isEnvironmentVariableName("NÄME"));
}

int main() { return RUN_TESTS(); }
