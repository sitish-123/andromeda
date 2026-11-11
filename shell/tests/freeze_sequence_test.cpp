#include "capture/freeze_sequence.h"
#include "tests/test_check.h"

#include <cstdint>
#include <variant>

namespace {

  using Sequence = freeze_capture::Sequence<int>;

  int captured(const Sequence::Step& step) {
    const auto* capture = std::get_if<freeze_capture::Capture<int>>(&step);
    return capture == nullptr ? -1 : capture->output;
  }

  // Output 1 is frozen and its next step is queued; a retrigger starts a new run before that step runs.
  void staleStepAfterRetriggerTakesNothing() {
    Sequence sequence;
    const std::uint64_t first = sequence.begin({1, 2});
    TEST_CHECK(captured(sequence.next(first)) == 1);

    const std::uint64_t second = sequence.begin({1, 2});
    TEST_CHECK(captured(sequence.next(second)) == 1);
    TEST_CHECK(std::holds_alternative<freeze_capture::Stale>(sequence.next(first)));

    TEST_CHECK(captured(sequence.next(second)) == 2);
    TEST_CHECK(std::holds_alternative<freeze_capture::Finished>(sequence.next(second)));
    TEST_CHECK(!sequence.capturing());
  }

  void abortDropsQueuedStep() {
    Sequence sequence;
    const std::uint64_t run = sequence.begin({1});
    sequence.abort();
    TEST_CHECK(std::holds_alternative<freeze_capture::Stale>(sequence.next(run)));
    TEST_CHECK(!sequence.capturing());
  }

} // namespace

int main() {
  staleStepAfterRetriggerTakesNothing();
  abortDropsQueuedStep();
  return 0;
}
