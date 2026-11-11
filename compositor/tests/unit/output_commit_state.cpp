#include "check.h"
#include "output/configured_commit.h"
#include "output/held_frame_phase.h"

#include <optional>
#include <random>

using andromeda::CommitOutcome;
using andromeda::CommitResult;
using andromeda::ConfiguredCommit;
using andromeda::ConfiguredRun;
using andromeda::HeldFrameEvent;
using andromeda::HeldFramePhase;
using andromeda::HeldFrameTimer;
using andromeda::PowerState;
using andromeda::RequestedMode;

namespace {

  // An output as Output drives the machine: `logical` is m_desktopEnabled/m_dpmsOff, `live` what the last applied
  // commit put on the connector. `oldest` is the logical state before the first request a pending run merged.
  struct OutputModel {
    ConfiguredCommit commit;
    PowerState logical{.desktopEnabled = true};
    PowerState live{.desktopEnabled = true};
    std::optional<PowerState> oldest;
    bool deferredFailed = false;

    std::optional<ConfiguredRun> request(PowerState next, bool stageGeometry, bool flipPending) {
      if (!oldest.has_value()) {
        oldest = logical;
      }
      const PowerState previous = logical;
      logical = next;
      return commit.request(stageGeometry, previous, flipPending);
    }

    void run(const ConfiguredRun& run, CommitOutcome outcome) {
      CHECK(oldest.has_value() && run.restore == *oldest);
      const PowerState requested = logical;
      const andromeda::ConfiguredFinish finished = commit.finish(run, outcome);
      CHECK_EQ(finished.result == CommitResult::Pending, outcome == CommitOutcome::Busy);
      CHECK_EQ(finished.result == CommitResult::Failed, outcome == CommitOutcome::Failed);
      CHECK_EQ(finished.restore.has_value(), outcome == CommitOutcome::Failed && !run.deferred);
      if (finished.result == CommitResult::Applied) {
        live = logical;
        deferredFailed = false;
      } else if (finished.restore.has_value()) {
        logical = *finished.restore;
      } else if (finished.result == CommitResult::Failed) {
        CHECK(logical == requested);
        deferredFailed = true;
      }
      if (finished.result != CommitResult::Pending) {
        oldest.reset();
      }
    }
  };

  CommitOutcome randomOutcome(std::mt19937& rng) {
    switch (rng() % 4) {
    case 0:
      return CommitOutcome::Failed;
    case 1:
      return CommitOutcome::Busy;
    default:
      return CommitOutcome::Ok;
    }
  }

} // namespace

ANDROMEDA_TEST(deferredApplyIsPendingNotApplied) {
  ConfiguredCommit commit;
  CHECK(!commit.request(true, {.desktopEnabled = true}, true).has_value());
  CHECK(std::holds_alternative<ConfiguredCommit::Queued>(commit.state()));
  CHECK(commit.beginFrame());
  const std::optional<ConfiguredRun> due = commit.takeDue();
  CHECK(due.has_value());
  CHECK(due.has_value() && due->stageGeometry);
  CHECK(!commit.endFrame().has_value());
  CHECK(std::holds_alternative<ConfiguredCommit::Idle>(commit.state()));
}

ANDROMEDA_TEST(deferredFailureKeepsTheRequestedState) {
  OutputModel output;
  CHECK(!output.request({.desktopEnabled = true, .dpmsOff = true}, false, true).has_value());
  const PowerState requested{.desktopEnabled = true, .dpmsOff = false};
  CHECK(!output.request(requested, true, true).has_value());
  CHECK(output.commit.beginFrame());
  const std::optional<ConfiguredRun> due = output.commit.takeDue();
  CHECK(due.has_value() && due->stageGeometry && due->deferred);
  if (due.has_value()) {
    output.run(*due, CommitOutcome::Failed);
  }
  (void)output.commit.endFrame();
  CHECK(output.logical == requested);
}

ANDROMEDA_TEST(synchronousFailureRestoresTheLastAppliedState) {
  OutputModel output;
  const PowerState applied = output.live;
  const std::optional<ConfiguredRun> run = output.request({.desktopEnabled = false}, true, false);
  CHECK(run.has_value());
  if (run.has_value()) {
    output.run(*run, CommitOutcome::Failed);
  }
  CHECK(output.logical == applied);
}

ANDROMEDA_TEST(busyCommitRetriesUntilItCommits) {
  OutputModel output;
  std::optional<ConfiguredRun> run = output.request(output.logical, true, false);
  for (int attempt = 0; attempt < 100 && run.has_value(); ++attempt) {
    output.run(*run, CommitOutcome::Busy);
    CHECK(std::holds_alternative<ConfiguredCommit::Queued>(output.commit.state()));
    CHECK(output.commit.beginFrame());
    run = output.commit.takeDue();
    CHECK(run.has_value() && run->stageGeometry);
    (void)output.commit.endFrame();
  }
  CHECK(run.has_value());
  if (run.has_value()) {
    output.run(*run, CommitOutcome::Ok);
  }
  CHECK(std::holds_alternative<ConfiguredCommit::Idle>(output.commit.state()));
}

ANDROMEDA_TEST(modeWaitsOnlyInsideTheFrame) {
  ConfiguredCommit commit;
  CHECK(!commit.requestMode({.width = 800, .height = 600}));
  CHECK(commit.beginFrame());
  CHECK(!commit.beginFrame());
  CHECK(commit.requestMode({.width = 800, .height = 600}));
  CHECK(commit.requestMode({.width = 1024, .height = 768}));
  const std::optional<RequestedMode> mode = commit.endFrame();
  CHECK(mode.has_value() && mode->width == 1024);
  CHECK(!commit.endFrame().has_value());
  CHECK(!commit.requestMode({.width = 800, .height = 600}));
}

ANDROMEDA_TEST(randomCommitSequencesKeepLogicalStateOnTheLiveState) {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    OutputModel output;
    std::optional<RequestedMode> lastMode;
    for (int step = 0; step < 200; ++step) {
      switch (rng() % 5) {
      case 0:
      case 1: {
        const PowerState next{.desktopEnabled = rng() % 4 != 0, .dpmsOff = rng() % 3 == 0};
        const bool flipPending = rng() % 2 == 0;
        const bool stageGeometry = rng() % 2 == 0;
        const ConfiguredCommit::State before = output.commit.state();
        const std::optional<ConfiguredRun> run = output.request(next, stageGeometry, flipPending);
        CHECK_EQ(run.has_value(), !flipPending);
        CHECK(output.commit.inFrame() == std::holds_alternative<ConfiguredCommit::InFrame>(before));
        if (run.has_value()) {
          output.run(*run, randomOutcome(rng));
        }
        break;
      }
      case 2: {
        const bool wasInFrame = output.commit.inFrame();
        const ConfiguredCommit::State before = output.commit.state();
        CHECK_EQ(output.commit.beginFrame(), !wasInFrame);
        if (wasInFrame) {
          CHECK(output.commit.state() == before);
          break;
        }
        if (const std::optional<ConfiguredRun> due = output.commit.takeDue()) {
          output.run(*due, randomOutcome(rng));
        }
        lastMode.reset();
        break;
      }
      case 3: {
        const RequestedMode mode{.width = static_cast<int>(rng() % 4000), .height = 600};
        const bool waits = output.commit.requestMode(mode);
        CHECK_EQ(waits, output.commit.inFrame());
        if (waits) {
          lastMode = mode;
        }
        break;
      }
      default: {
        const bool wasInFrame = output.commit.inFrame();
        const ConfiguredCommit::State before = output.commit.state();
        const std::optional<RequestedMode> mode = output.commit.endFrame();
        CHECK(!output.commit.inFrame());
        if (!wasInFrame) {
          CHECK(output.commit.state() == before);
          CHECK(!mode.has_value());
        } else {
          CHECK(mode == lastMode);
        }
        lastMode.reset();
        break;
      }
      }
      if (std::holds_alternative<ConfiguredCommit::Idle>(output.commit.state()) && !output.deferredFailed) {
        CHECK(output.logical == output.live);
      }
    }
  }
}

ANDROMEDA_TEST(randomHeldFrameSequencesKeepTheTimerOnlyWhileHolding) {
  for (unsigned seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    HeldFramePhase phase = HeldFramePhase::Holding;
    bool timer = true;
    for (int step = 0; step < 40; ++step) {
      const auto event = static_cast<HeldFrameEvent>(rng() % 6);
      const std::optional<andromeda::HeldFrameStep> next = andromeda::heldFrameNext(phase, event);
      if (!next.has_value()) {
        CHECK(phase == HeldFramePhase::Gone || event != HeldFrameEvent::Dropped);
        continue;
      }
      CHECK(phase != HeldFramePhase::Gone);
      if (next->timer == HeldFrameTimer::Remove) {
        CHECK(timer);
        timer = false;
      } else if (next->timer == HeldFrameTimer::Rearm) {
        CHECK(timer);
      }
      if (next->phase == HeldFramePhase::Crossing) {
        CHECK(event == HeldFrameEvent::TimerFired || event == HeldFrameEvent::GreeterShown);
      }
      CHECK(event != HeldFrameEvent::TimerFiredGreeterEmpty || next->phase == HeldFramePhase::AwaitingGreeter);
      phase = next->phase;
      CHECK_EQ(timer, andromeda::heldFrameTimerArmed(phase));
    }
    CHECK(!andromeda::heldFrameNext(HeldFramePhase::Gone, HeldFrameEvent::TimerFired).has_value());
  }
}

ANDROMEDA_TEST(heldFrameWaitsForTheGreeterToShowSomething) {
  using andromeda::heldFrameNext;
  const auto fired = heldFrameNext(HeldFramePhase::Holding, HeldFrameEvent::TimerFiredGreeterEmpty);
  CHECK(fired.has_value() && fired->phase == HeldFramePhase::AwaitingGreeter && fired->timer == HeldFrameTimer::Remove);
  CHECK(!heldFrameNext(HeldFramePhase::AwaitingGreeter, HeldFrameEvent::TimerFired).has_value());
  CHECK(!heldFrameNext(HeldFramePhase::Holding, HeldFrameEvent::GreeterShown).has_value());
  const auto shown = heldFrameNext(HeldFramePhase::AwaitingGreeter, HeldFrameEvent::GreeterShown);
  CHECK(shown.has_value() && shown->phase == HeldFramePhase::Crossing && shown->timer == HeldFrameTimer::Keep);
}

int main() { return RUN_TESTS(); }
