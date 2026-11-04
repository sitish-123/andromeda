#include "server/tracker_motion.h"

#include "check.h"

using andromeda::TrackerMotion;

namespace {
  // Drags `distance` px over `steps` samples 8 ms apart, releases, and runs inertia until idle.
  double flingAndSettle(TrackerMotion& tracker, double distance, int steps) {
    uint32_t now = 1000;
    tracker.begin(now);
    for (int i = 0; i < steps; ++i) {
      tracker.update(distance / steps, now += 8);
    }
    tracker.end(now);
    for (int i = 0; i < 500 && tracker.phase() != TrackerMotion::Phase::Idle; ++i) {
      tracker.tick(now += 16);
    }
    return tracker.position();
  }
} // namespace

// A short fast flick and a slow drag of the same length land on different snaps: the release speed decides.
ANDROMEDA_TEST(releaseSpeedPicksTheSnap) {
  TrackerMotion fast;
  fast.setRange(0.0, 400.0);
  fast.addSnap(0.0);
  fast.addSnap(400.0);
  CHECK(flingAndSettle(fast, 100.0, 4) == 400.0);

  TrackerMotion slow;
  slow.setRange(0.0, 400.0);
  slow.addSnap(0.0);
  slow.addSnap(400.0);
  CHECK(flingAndSettle(slow, 100.0, 60) == 0.0);
}

// Dragging past the range moves the position less than the finger, and the release still rests inside the range.
ANDROMEDA_TEST(overscrollRubberBandsAndSettlesInRange) {
  TrackerMotion tracker;
  tracker.setRange(0.0, 400.0);
  uint32_t now = 1000;
  tracker.begin(now);
  for (int i = 0; i < 50; ++i) {
    tracker.update(-10.0, now += 16);
  }
  CHECK(tracker.position() < 0.0 && tracker.position() > -TrackerMotion::kOverscrollPx);
  tracker.end(now += 200);
  for (int i = 0; i < 500 && tracker.phase() != TrackerMotion::Phase::Idle; ++i) {
    tracker.tick(now += 16);
  }
  CHECK(tracker.position() == 0.0);

  const auto events = tracker.takeEvents();
  CHECK(events.size() == 3);
  CHECK(events[0].kind == TrackerMotion::Event::Kind::Interacting);
  CHECK(events[1].kind == TrackerMotion::Event::Kind::Inertia && events[1].position == 0.0);
  CHECK(events[2].kind == TrackerMotion::Event::Kind::Idle);
}

int main() { return RUN_TESTS(); }
