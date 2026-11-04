#include "check.h"
#include "util/listener.h"

#include <memory>

using namespace andromeda;

namespace {
  void storePayload(int* out, void* data) { *out = *static_cast<int*>(data); }
} // namespace

ANDROMEDA_TEST(listenerRunsOnEmitWithPayload) {
  wl_signal signal;
  wl_signal_init(&signal);
  int payload = 7;
  int seen = 0;
  Listener listener;
  listener.connect(&signal, &seen, storePayload);
  CHECK(listener.connected());
  wl_signal_emit_mutable(&signal, &payload);
  CHECK_EQ(seen, 7);
}

ANDROMEDA_TEST(destroyedListenerNeverRuns) {
  wl_signal signal;
  wl_signal_init(&signal);
  int runs = 0;
  {
    Listener listener;
    listener.connect(&signal, [&](void*) { ++runs; });
  }
  wl_signal_emit_mutable(&signal, nullptr);
  CHECK_EQ(runs, 0);
  CHECK(wl_list_empty(&signal.listener_list) != 0);
}

ANDROMEDA_TEST(listenerDisconnectsItselfDuringEmit) {
  wl_signal signal;
  wl_signal_init(&signal);
  int runs = 0;
  int after = 0;
  Listener first;
  Listener second;
  first.connect(&signal, [&](void*) {
    ++runs;
    first.disconnect();
  });
  second.connect(&signal, [&](void*) { ++after; });
  wl_signal_emit_mutable(&signal, nullptr);
  wl_signal_emit_mutable(&signal, nullptr);
  CHECK_EQ(runs, 1);
  CHECK_EQ(after, 2);
  CHECK(!first.connected());
}

ANDROMEDA_TEST(listenerDisconnectsAnotherDuringEmit) {
  wl_signal signal;
  wl_signal_init(&signal);
  int victimRuns = 0;
  int survivorRuns = 0;
  Listener killer;
  Listener victim;
  Listener survivor;
  killer.connect(&signal, [&](void*) { victim.disconnect(); });
  victim.connect(&signal, [&](void*) { ++victimRuns; });
  survivor.connect(&signal, [&](void*) { ++survivorRuns; });
  wl_signal_emit_mutable(&signal, nullptr);
  CHECK_EQ(victimRuns, 0);
  CHECK_EQ(survivorRuns, 1);
}

ANDROMEDA_TEST(listenerReconnectsToSecondSignal) {
  wl_signal a;
  wl_signal b;
  wl_signal_init(&a);
  wl_signal_init(&b);
  int runs = 0;
  Listener listener;
  listener.connect(&a, [&](void*) { ++runs; });
  listener.connect(&b, [&](void*) { runs += 10; });
  wl_signal_emit_mutable(&a, nullptr);
  CHECK_EQ(runs, 0);
  wl_signal_emit_mutable(&b, nullptr);
  CHECK_EQ(runs, 10);
}

int main() { return RUN_TESTS(); }
