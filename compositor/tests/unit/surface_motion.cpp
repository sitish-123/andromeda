#include "server/surface_motion.h"

#include "check.h"

using andromeda::SurfaceMotion;
using Kind = SurfaceMotion::Event::Kind;

ANDROMEDA_TEST(retargetKeepsVelocityAndReportsEachSerial) {
  SurfaceMotion motion;
  motion.animate(1, SurfaceMotion::Property::Translate, SurfaceMotion::Curve::SpringStandard, {100.0, 0.0, 0.0, 0.0});
  uint32_t now = 1000;
  motion.tick(now);
  for (int i = 0; i < 3; ++i) {
    motion.tick(now += 16);
  }
  const double before = motion.value(SurfaceMotion::Property::Translate)[0];
  CHECK(before > 0.0 && before < 100.0);

  // Retarget behind the current position: a spring that kept its velocity still moves forward on the next frame.
  motion.animate(2, SurfaceMotion::Property::Translate, SurfaceMotion::Curve::SpringStandard, {0.0, 0.0, 0.0, 0.0});
  motion.tick(now += 16);
  motion.tick(now += 16);
  CHECK(motion.value(SurfaceMotion::Property::Translate)[0] > before);

  for (int i = 0; i < 300 && motion.animating(); ++i) {
    motion.tick(now += 16);
  }
  CHECK(!motion.animating());
  CHECK(motion.value(SurfaceMotion::Property::Translate)[0] == 0.0);

  const auto events = motion.takeEvents();
  CHECK(events.size() == 4);
  CHECK(events[0].kind == Kind::Started && events[0].serial == 1);
  CHECK(events[1].kind == Kind::Done && events[1].serial == 1);
  CHECK(events[1].reason == SurfaceMotion::Reason::Retargeted);
  CHECK(events[2].kind == Kind::Started && events[2].serial == 2);
  CHECK(events[3].kind == Kind::Done && events[3].serial == 2);
  CHECK(events[3].reason == SurfaceMotion::Reason::Finished);
}

int main() { return RUN_TESTS(); }
