// The shared worker returns results on the main loop only while the owner lives, and exits when idle.

#include "core/deferred_call.h"
#include "core/shared_worker.h"
#include "tests/test_check.h"

#include <chrono>
#include <memory>
#include <thread>

namespace {

  // Runs main-loop callbacks until `done` holds or the timeout passes.
  template <typename Pred> bool pumpUntil(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      DeferredCall::drainWakeFd();
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
      std::this_thread::yield();
    }
    return true;
  }

} // namespace

int main() {
  SharedWorker worker(std::chrono::milliseconds(50));

  auto alive = std::make_shared<int>(0);
  int got = 0;
  worker.post<int>(alive, [] { return 42; }, [&got](int value) { got = value; });
  TEST_CHECK(pumpUntil([&] { return got == 42; }));

  auto gone = std::make_shared<int>(0);
  bool dropped = true;
  worker.post<int>(std::weak_ptr<void>(gone), [] { return 1; }, [&dropped](int) { dropped = false; });
  gone.reset();
  // Idle exit means every completion has been posted; pumping runs them.
  TEST_CHECK(pumpUntil([&] { return !worker.running(); }));
  for (auto& fn : DeferredCall::takePending()) {
    fn();
  }
  TEST_CHECK(dropped);

  got = 0;
  worker.post<int>(alive, [] { return 7; }, [&got](int value) { got = value; });
  TEST_CHECK(pumpUntil([&] { return got == 7; }));
  return 0;
}
