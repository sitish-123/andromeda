#include "check.h"
#include "util/tracked.h"

#include <memory>
#include <utility>
#include <vector>

using namespace andromeda;

namespace {
  struct Thing : Trackable {};
} // namespace

ANDROMEDA_TEST(refIsNullAfterInvalidate) {
  Thing thing;
  Ref<Thing> ref(&thing);
  CHECK(ref.get() == &thing);
  CHECK(static_cast<bool>(ref));
  thing.invalidateRefs();
  CHECK(ref.get() == nullptr);
  CHECK(!ref);
}

ANDROMEDA_TEST(refIsNullAfterDestroy) {
  auto thing = std::make_unique<Thing>();
  Ref<Thing> ref(thing.get());
  Ref<Thing> second(thing.get());
  thing.reset();
  CHECK(!ref);
  CHECK(!second);
}

ANDROMEDA_TEST(copiesAndMovesTrackTheTarget) {
  Thing thing;
  Ref<Thing> original(&thing);
  Ref<Thing> copy(original);
  Ref<Thing> moved(std::move(original));
  Ref<Thing> assigned;
  assigned = copy;
  CHECK(copy.get() == &thing);
  CHECK(moved.get() == &thing);
  CHECK(assigned.get() == &thing);
  thing.invalidateRefs();
  CHECK(!copy);
  CHECK(!moved);
  CHECK(!assigned);
}

ANDROMEDA_TEST(refDestroyedBeforeTargetLeavesOthersTracked) {
  Thing thing;
  Ref<Thing> kept(&thing);
  {
    Ref<Thing> shortLived(&thing);
    Ref<Thing> alsoShort(&thing);
  }
  CHECK(kept.get() == &thing);
  thing.invalidateRefs();
  CHECK(!kept);
}

ANDROMEDA_TEST(reassignedRefStopsTrackingTheOldTarget) {
  Thing a;
  Thing b;
  Ref<Thing> ref(&a);
  ref = &b;
  a.invalidateRefs();
  CHECK(ref.get() == &b);
  ref = nullptr;
  b.invalidateRefs();
  CHECK(!ref);
}

ANDROMEDA_TEST(refsInAContainerSurviveInvalidateOfOneTarget) {
  Thing a;
  Thing b;
  std::vector<Ref<Thing>> refs;
  for (int i = 0; i < 8; ++i) {
    refs.emplace_back(i % 2 == 0 ? &a : &b);
  }
  a.invalidateRefs();
  for (int i = 0; i < 8; ++i) {
    CHECK((refs[i].get() == nullptr) == (i % 2 == 0));
  }
  refs.clear();
  b.invalidateRefs();
}

int main() { return RUN_TESTS(); }
