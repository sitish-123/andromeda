#include "input/key_pipeline.h"

#include "check.h"

#include <string>
#include <vector>

using andromeda::KeyEvent;
using andromeda::KeyPipeline;
using andromeda::KeyResult;
using andromeda::KeyStage;

namespace {

  constexpr KeyStage kAll[] = {
      KeyStage::AccessibilityFilter,
      KeyStage::KeyboardMonitor,
      KeyStage::Bindings,
      KeyStage::SnippetMatcher,
      KeyStage::InputMethodGrab,
      KeyStage::FocusedClient,
  };

  void install(KeyPipeline& pipeline, std::vector<int>& seen, KeyStage slot, KeyResult result) {
    pipeline.set(slot, [&seen, slot, result](KeyEvent&) {
      seen.push_back(static_cast<int>(slot));
      return result;
    });
  }

} // namespace

ANDROMEDA_TEST(everyStageSeesTheEventInDocumentedOrder) {
  KeyPipeline pipeline;
  std::vector<int> seen;
  for (KeyStage slot : kAll) {
    install(pipeline, seen, slot, KeyResult::Pass);
  }
  KeyEvent event;
  CHECK(pipeline.run(event) == KeyResult::Pass);
  CHECK_EQ(seen, (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

ANDROMEDA_TEST(consumeStopsLaterStages) {
  KeyPipeline pipeline;
  std::vector<int> seen;
  for (KeyStage slot : kAll) {
    install(pipeline, seen, slot, slot == KeyStage::Bindings ? KeyResult::Consume : KeyResult::Pass);
  }
  KeyEvent event;
  CHECK(pipeline.run(event) == KeyResult::Consume);
  CHECK_EQ(seen, (std::vector<int>{0, 1, 2}));
}

ANDROMEDA_TEST(replaceHandsTheRewrittenEventToTheNextStage) {
  KeyPipeline pipeline;
  uint32_t seenByClient = 0;
  pipeline.set(KeyStage::KeyboardMonitor, [](KeyEvent& event) {
    event.keycode = 42;
    return KeyResult::Replace;
  });
  pipeline.set(KeyStage::FocusedClient, [&seenByClient](KeyEvent& event) {
    seenByClient = event.keycode;
    return KeyResult::Consume;
  });
  KeyEvent event{.time_msec = 1, .keycode = 7, .state = 1};
  CHECK(pipeline.run(event) == KeyResult::Consume);
  CHECK_EQ(seenByClient, uint32_t{42});
}

ANDROMEDA_TEST(emptySlotsPass) {
  KeyPipeline pipeline;
  KeyEvent event;
  CHECK(pipeline.run(event) == KeyResult::Pass);
}

ANDROMEDA_TEST(syntheticEntrySkipsStagesUpToBindings) {
  KeyPipeline pipeline;
  std::vector<int> seen;
  for (KeyStage slot : kAll) {
    install(pipeline, seen, slot, KeyResult::Pass);
  }
  KeyEvent event;
  pipeline.runAfterBindings(event);
  CHECK_EQ(seen, (std::vector<int>{3, 4, 5}));
}

int main() { return RUN_TESTS(); }
