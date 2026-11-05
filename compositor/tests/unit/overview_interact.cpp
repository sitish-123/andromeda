#include "check.h"
#include "overview/interact.h"

#include <array>
#include <string>
#include <xkbcommon/xkbcommon.h>

using namespace andromeda::ov;

ANDROMEDA_TEST(filterMatchesTitleOrAppIdIgnoringCase) {
  CHECK(filterMatches("", "Anything", "app"));
  CHECK(filterMatches("fire", "Mozilla Firefox", "org.mozilla.firefox"));
  CHECK(filterMatches("KITTY", "~/src", "kitty"));
  CHECK(!filterMatches("term", "Mozilla Firefox", "firefox"));
}

ANDROMEDA_TEST(filterTextSkipsControlKeysAndLeadingSpace) {
  CHECK_EQ(filterText(XKB_KEY_a, true), std::string("a"));
  CHECK_EQ(filterText(XKB_KEY_A, true), std::string("A"));
  CHECK_EQ(filterText(XKB_KEY_eacute, true), std::string("\xC3\xA9"));
  CHECK(filterText(XKB_KEY_space, true).empty());
  CHECK_EQ(filterText(XKB_KEY_space, false), std::string(" "));
  CHECK(filterText(XKB_KEY_Return, false).empty());
  CHECK(filterText(XKB_KEY_BackSpace, false).empty());
  CHECK(filterText(XKB_KEY_F2, false).empty());
}

ANDROMEDA_TEST(popRemovesWholeUtf8Character) {
  std::string filter = "a\xC3\xA9";
  popFilterCharacter(filter);
  CHECK_EQ(filter, std::string("a"));
  popFilterCharacter(filter);
  CHECK(filter.empty());
  popFilterCharacter(filter);
  CHECK(filter.empty());
}

ANDROMEDA_TEST(digitsMapToWorkspaces) {
  CHECK_EQ(workspaceDigit(XKB_KEY_1), 0);
  CHECK_EQ(workspaceDigit(XKB_KEY_9), 8);
  CHECK_EQ(workspaceDigit(XKB_KEY_KP_3), 2);
  CHECK_EQ(workspaceDigit(XKB_KEY_0), -1);
  CHECK_EQ(workspaceDigit(XKB_KEY_a), -1);
}

ANDROMEDA_TEST(pickPrefersAlignedCandidates) {
  // Right of the origin: one far but level, one near but well below.
  const std::array<Point, 3> cards{{{400, 0}, {150, 200}, {-300, 0}}};
  CHECK_EQ(pickInDirection(cards, {0, 0}, Direction::Right), 0);
  CHECK_EQ(pickInDirection(cards, {0, 0}, Direction::Left), 2);
  CHECK_EQ(pickInDirection(cards, {0, 0}, Direction::Down), 1);
  CHECK_EQ(pickInDirection(cards, {0, 0}, Direction::Up), -1);
}

ANDROMEDA_TEST(cycleWraps) {
  CHECK_EQ(cycleIndex(3, 2, 1), 0u);
  CHECK_EQ(cycleIndex(3, 0, -1), 2u);
  CHECK_EQ(cycleIndex(3, -1, 1), 0u);
  CHECK_EQ(cycleIndex(3, -1, -1), 2u);
  CHECK_EQ(cycleIndex(0, -1, 1), 0u);
}

int main() { return RUN_TESTS(); }
