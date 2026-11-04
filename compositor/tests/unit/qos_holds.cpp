#include "qos/holds.h"

#include "check.h"

using andromeda::qos::Decision;
using andromeda::qos::Holds;
using andromeda::qos::Tier;

ANDROMEDA_TEST(theMostThrottledOfPolicyAndHoldsWins) {
  Holds holds;
  CHECK(holds.holdTier(":1.5", "app-a.scope", Tier::HiddenLong, "thermal", true).has_value());
  CHECK(holds.apply("app-a.scope", {.tier = Tier::Hidden}, false).tier == Tier::HiddenLong);
  CHECK(holds.apply("app-b.scope", {.tier = Tier::Hidden}, false).tier == Tier::Hidden);
  CHECK(holds.holdTier(":1.5", "app-c.scope", Tier::Hidden, "x", true).has_value());
  CHECK(holds.apply("app-c.scope", {.tier = Tier::HiddenLong}, false).tier == Tier::HiddenLong);
}

ANDROMEDA_TEST(anExemptAppIgnoresTierAndFreezeHolds) {
  Holds holds;
  (void)holds.holdTier(":1.5", "app-call.scope", Tier::HiddenLong, "thermal", true);
  (void)holds.holdFreeze(":1.5", "app-call.scope", "memory", true);
  const Decision exempt = holds.apply("app-call.scope", {.tier = Tier::Focus}, true);
  CHECK(exempt.tier == Tier::Focus);
  CHECK(!exempt.frozen);
  CHECK(holds.apply("app-call.scope", {.tier = Tier::Hidden}, false).frozen);
}

ANDROMEDA_TEST(aHoldOnAnUnknownUnitFails) {
  Holds holds;
  CHECK(!holds.holdFreeze(":1.5", "app-new.scope", "memory", false).has_value());
  CHECK(holds.empty());
}

ANDROMEDA_TEST(onlyTheOwnerReleasesAndLeavingTheBusEndsItsHolds) {
  Holds holds;
  const auto id = holds.holdFreeze(":1.5", "app-a.scope", "memory", true);
  (void)holds.holdTier(":1.5", "app-b.scope", Tier::HiddenLong, "drainer", true);
  (void)holds.holdTier(":1.9", "app-b.scope", Tier::Hidden, "other", true);
  CHECK(!holds.release(*id, ":1.9"));
  CHECK(holds.release(*id, ":1.5"));
  CHECK(!holds.apply("app-a.scope", {.tier = Tier::Hidden}, false).frozen);
  CHECK(holds.releaseOwner(":1.5"));
  CHECK(holds.apply("app-b.scope", {.tier = Tier::Visible}, false).tier == Tier::Hidden);
  holds.forget("app-b.scope");
  CHECK(holds.empty());
}

int main() { return RUN_TESTS(); }
