#include "shell/live_activity/island_queue.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>
#include <tuple>

namespace {

  struct Banner {
    std::uint32_t id = 0;
    bool sticky = false;
  };
  struct Osd {
    int value = 0;
  };
  using Queue = IslandQueue<Banner, Osd>;
  using Mode = Queue::Mode;
  using TimerCmd = Queue::TimerCmd;

  struct FakeTimer {
    bool running = false;
    void apply(TimerCmd cmd) {
      if (cmd == TimerCmd::Start) {
        running = true;
      } else if (cmd == TimerCmd::Stop) {
        running = false;
      }
    }
  };

  void testRandomSequencesKeepInvariants() {
    std::mt19937 rng(0x15a1);
    for (int run = 0; run < 2000; ++run) {
      Queue queue;
      FakeTimer bannerTimer;
      FakeTimer osdTimer;
      std::uint32_t nextId = 1;
      bool active = false;
      bool timedCall = false;
      bool hovered = false;
      bool restless = false;
      for (int step = 0; step < 60; ++step) {
        Queue::Step result;
        const auto before = std::tuple(
            queue.osd().has_value(), queue.banner().has_value() ? queue.banner()->id : 0U, queue.mode()
        );
        switch (std::uniform_int_distribution<int>(0, 8)(rng)) {
        case 0:
          result = queue.showOsd(Osd{.value = step});
          break;
        case 1:
          if (!osdTimer.running) {
            continue;
          }
          osdTimer.running = false;
          result = queue.endOsd();
          break;
        case 2:
          result = queue.endOsd();
          break;
        case 3:
          active = (rng() & 1U) != 0;
          timedCall = (rng() & 1U) != 0;
          hovered = (rng() & 1U) != 0;
          restless = (rng() & 1U) != 0;
          result = queue.setActivity(activityHidesBanner(active, timedCall, hovered, restless));
          break;
        case 4:
          result = queue.push(Banner{.id = nextId++, .sticky = (rng() % 4) == 0});
          break;
        case 5:
          if (!bannerTimer.running) {
            continue;
          }
          bannerTimer.running = false;
          result = queue.endBanner();
          break;
        case 6:
          result = queue.endBanner();
          break;
        case 7: {
          const std::uint32_t id = nextId > 1 ? 1 + (rng() % (nextId - 1)) : 0;
          result = queue.dropIf([id](const Banner& b) { return b.id == id; });
          break;
        }
        default: {
          const std::uint32_t id = nextId > 1 ? 1 + (rng() % (nextId - 1)) : 0;
          (void)queue.update(Banner{.id = id, .sticky = (rng() & 1U) != 0}, result);
          break;
        }
        }
        bannerTimer.apply(result.banner);
        osdTimer.apply(result.osd);

        // The banner timer runs exactly while the banner shows and can expire; a compact timed call shows it.
        const bool shown = !queue.osd().has_value() && queue.banner().has_value()
                           && (!active || (timedCall && !hovered && !restless));
        const bool timed = shown && !queue.banner()->sticky;
        TEST_CHECK(bannerTimer.running == timed);
        TEST_CHECK(queue.bannerTimerRunning() == timed);
        TEST_CHECK(osdTimer.running == queue.osd().has_value());
        // A free island never leaves banners waiting in the queue.
        TEST_CHECK(queue.osd().has_value() || queue.banner().has_value() || queue.queued().empty());
        TEST_CHECK(!queue.osd().has_value() || !queue.banner().has_value());
        // A step that reports no change leaves the face as it was.
        const auto after = std::tuple(
            queue.osd().has_value(), queue.banner().has_value() ? queue.banner()->id : 0U, queue.mode()
        );
        TEST_CHECK(result.changed || before == after);
      }
    }
  }

  void testBannerWaitsBehindActivity() {
    Queue queue;
    (void)queue.setActivity(true);
    const auto pushed = queue.push(Banner{.id = 1});
    TEST_CHECK(pushed.banner != TimerCmd::Start);
    TEST_CHECK(queue.mode() == Mode::Activity && queue.banner().has_value());
    TEST_CHECK(queue.setActivity(false).banner == TimerCmd::Start);
  }

  void testCompactTimedCallShowsAndExpiresTheBanner() {
    Queue queue;
    (void)queue.setActivity(activityHidesBanner(true, true, false, false));
    TEST_CHECK(queue.push(Banner{.id = 1}).banner == TimerCmd::Start);
    TEST_CHECK(queue.setActivity(activityHidesBanner(true, true, true, false)).banner == TimerCmd::Stop);
  }

  void testDroppedOsdDrainsTheQueue() {
    Queue queue;
    (void)queue.showOsd(Osd{});
    (void)queue.push(Banner{.id = 1});
    const auto ended = queue.endOsd();
    TEST_CHECK(ended.osd == TimerCmd::Stop && ended.banner == TimerCmd::Start);
    TEST_CHECK(queue.mode() == Mode::Banner && queue.banner()->id == 1);
  }

} // namespace

int main() {
  testRandomSequencesKeepInvariants();
  testBannerWaitsBehindActivity();
  testCompactTimedCallShowsAndExpiresTheBanner();
  testDroppedOsdDrainsTheQueue();
  return 0;
}
