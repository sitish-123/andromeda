#include "qos/policy.h"

#include "check.h"

using andromeda::qos::AppInputs;
using andromeda::qos::kHiddenDelayMs;
using andromeda::qos::kHiddenLongDelayMs;
using andromeda::qos::Rule;
using andromeda::qos::ScopePolicy;
using andromeda::qos::Tier;

namespace {
  constexpr std::uint64_t kStart = 1'000'000;
  constexpr std::uint64_t kLongAt = kStart + kHiddenDelayMs + kHiddenLongDelayMs;
  const AppInputs kHidden{};
} // namespace

ANDROMEDA_TEST(focusedAndVisibleScopesKeepTheirTiers) {
  ScopePolicy policy;
  CHECK(policy.update({.focused = true}, true, kStart).tier == Tier::Focus);
  CHECK(policy.update({.visible = true}, true, kStart).tier == Tier::Visible);
  CHECK(!policy.update({.visible = true}, true, kStart).recheckAtMs.has_value());
}

ANDROMEDA_TEST(aHiddenScopeDemotesToHiddenAfterFiveSeconds) {
  ScopePolicy policy;
  const auto first = policy.update(kHidden, false, kStart);
  CHECK(first.tier == Tier::Visible);
  CHECK(first.recheckAtMs == kStart + kHiddenDelayMs);
  CHECK(policy.update(kHidden, false, kStart + kHiddenDelayMs - 1).tier == Tier::Visible);
  CHECK(policy.update(kHidden, false, kStart + kHiddenDelayMs).tier == Tier::Hidden);
}

ANDROMEDA_TEST(hiddenLongNeedsTenMinutesOnBattery) {
  ScopePolicy onAc;
  onAc.update(kHidden, false, kStart);
  const auto ac = onAc.update(kHidden, false, kLongAt + 1);
  CHECK(ac.tier == Tier::Hidden);
  CHECK(!ac.recheckAtMs.has_value());

  ScopePolicy onBattery;
  onBattery.update(kHidden, true, kStart);
  const auto hidden = onBattery.update(kHidden, true, kStart + kHiddenDelayMs);
  CHECK(hidden.tier == Tier::Hidden);
  CHECK(hidden.recheckAtMs == kLongAt);
  CHECK(onBattery.update(kHidden, true, kLongAt - 1).tier == Tier::Hidden);
  CHECK(onBattery.update(kHidden, true, kLongAt).tier == Tier::HiddenLong);
  // Unplugging the charger after a long hide moves to T3 at once.
  CHECK(onAc.update(kHidden, true, kLongAt + 1).tier == Tier::HiddenLong);
}

ANDROMEDA_TEST(exemptScopesNeverDropBelowVisible) {
  for (const AppInputs inputs :
       {AppInputs{.audio = true}, AppInputs{.call = true}, AppInputs{.idleInhibitor = true},
        AppInputs{.screenCast = true}, AppInputs{.rule = Rule::NeverThrottle}}) {
    ScopePolicy policy;
    policy.update(inputs, true, kStart);
    const auto decision = policy.update(inputs, true, kLongAt + 1);
    CHECK(decision.tier == Tier::Visible);
    CHECK(!decision.frozen);
  }
}

ANDROMEDA_TEST(onlyFreezeWhenHiddenFreezesAndIntentThaws) {
  ScopePolicy normal;
  normal.update(kHidden, true, kStart);
  CHECK(!normal.update(kHidden, true, kLongAt).frozen);

  ScopePolicy freezing;
  const AppInputs rule{.rule = Rule::FreezeWhenHidden};
  freezing.update(rule, true, kStart);
  CHECK(!freezing.update(rule, true, kStart + kHiddenDelayMs).frozen);
  CHECK(freezing.update(rule, true, kLongAt).frozen);
  const auto reached = freezing.update({.thawIntent = true, .rule = Rule::FreezeWhenHidden}, true, kLongAt + 1);
  CHECK(!reached.frozen);
}

ANDROMEDA_TEST(promotionIsImmediateAndRestartsTheHiddenClock) {
  ScopePolicy policy;
  policy.update(kHidden, true, kStart);
  CHECK(policy.update(kHidden, true, kLongAt).tier == Tier::HiddenLong);
  CHECK(policy.update({.focused = true}, true, kLongAt + 1).tier == Tier::Focus);
  // Hidden again: the 5 s delay starts over.
  const auto again = policy.update(kHidden, true, kLongAt + 2);
  CHECK(again.tier == Tier::Visible);
  CHECK(again.recheckAtMs == kLongAt + 2 + kHiddenDelayMs);
}

int main() { return RUN_TESTS(); }
