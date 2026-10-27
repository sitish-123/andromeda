#include "check.h"
#include "output/frame_schedule.h"

using andromeda::OutputFrameFollowup;
using andromeda::outputFrameFollowup;
using andromeda::TearingCommitRecovery;

ANDROMEDA_TEST(nestedBackendCanScheduleFrames) {
  CHECK_EQ(outputFrameFollowup(false, nullptr, false, true), OutputFrameFollowup::Schedule);
}

ANDROMEDA_TEST(activeNativeSessionCanScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::Schedule);
}

ANDROMEDA_TEST(inactiveNativeSessionCannotScheduleFrames) {
  wlr_session session{};
  session.active = false;
  CHECK_EQ(outputFrameFollowup(false, &session, false, true), OutputFrameFollowup::None);
}

ANDROMEDA_TEST(stoppingCompositorCannotScheduleFrames) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(true, &session, false, true), OutputFrameFollowup::None);
}

ANDROMEDA_TEST(failedCommitUsesDelayedRetry) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, false), OutputFrameFollowup::RetryDelayed);
}

ANDROMEDA_TEST(failedAnimatedFrameDoesNotScheduleImmediately) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, true, true), OutputFrameFollowup::RetryDelayed);
}

ANDROMEDA_TEST(idleSuccessfulFrameNeedsNoFollowup) {
  wlr_session session{};
  session.active = true;
  CHECK_EQ(outputFrameFollowup(false, &session, false, false), OutputFrameFollowup::None);
}

ANDROMEDA_TEST(failedTearingCommitForcesARegularRecoveryCommit) {
  TearingCommitRecovery recovery;

  CHECK(recovery.requestTearing(true));
  recovery.recordCommit(true, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));
}

ANDROMEDA_TEST(failedRegularRecoveryStaysRegularUntilItSucceeds) {
  TearingCommitRecovery recovery;
  recovery.recordCommit(true, false);
  recovery.recordCommit(false, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));

  recovery.recordCommit(false, true);
  CHECK(!recovery.regularCommitPending());
  CHECK(recovery.requestTearing(true));
}

ANDROMEDA_TEST(rejectedTearingTestKeepsFailedRegularFallbackOnTheRegularPath) {
  TearingCommitRecovery recovery;
  recovery.forceRegularCommit();
  recovery.recordCommit(false, false);

  CHECK(recovery.regularCommitPending());
  CHECK(!recovery.requestTearing(true));

  recovery.recordCommit(false, true);
  CHECK(recovery.requestTearing(true));
}

ANDROMEDA_TEST(ineligibleFramesNeverRequestTearing) {
  TearingCommitRecovery recovery;
  CHECK(!recovery.requestTearing(false));
  recovery.recordCommit(false, true);
  CHECK(!recovery.regularCommitPending());
}

ANDROMEDA_TEST(effectFrameDelayFollowsRefreshOrCapsAtMaxFps) {
  CHECK_EQ(andromeda::effectFrameDelayMs(0, 1000, 990), uint64_t{0});
  CHECK_EQ(andromeda::effectFrameDelayMs(60, 1000, 1000), uint64_t{17});
  CHECK_EQ(andromeda::effectFrameDelayMs(60, 1010, 1000), uint64_t{7});
  CHECK_EQ(andromeda::effectFrameDelayMs(60, 1100, 1000), uint64_t{1});
  CHECK_EQ(andromeda::effectFrameDelayMs(1, 1000, 1000), uint64_t{1000});
}

ANDROMEDA_TEST(effectFrameDelayWaitsAFullIntervalWhenTheLastFrameIsAhead) {
  CHECK_EQ(andromeda::effectFrameDelayMs(60, 990, 1000), uint64_t{17});
}

ANDROMEDA_TEST(effectFrameDelayNeverDropsBelowOneMillisecond) {
  CHECK_EQ(andromeda::effectFrameDelayMs(2000, 1000, 1000), uint64_t{1});
}

int main() { return RUN_TESTS(); }
