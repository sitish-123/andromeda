#include "shell/lockscreen/lock_client.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>

namespace {

  using State = LockClientState;
  using Notice = State::Notice;

  // The six flags LockClient kept before LockClientState, with their update rules.
  struct FlagModel {
    bool resolved = false;
    bool owned = false;
    bool seen = false;
    bool requested = false;
    bool active = false;
    bool locked = false;
    std::string reason;

    struct Out {
      Notice notice = Notice::None;
      bool resolvedNow = false;
      bool readState = false;
      std::optional<std::string> sendLock;
    };

    Notice applyState(bool nowActive, bool nowLocked) {
      const bool wasLocked = locked;
      const bool wasActive = requested || active;
      active = nowActive;
      locked = nowLocked;
      if (nowLocked || !nowActive) {
        requested = false;
      }
      if (nowLocked && !wasLocked) {
        return Notice::Locked;
      }
      if (!nowActive && wasLocked) {
        return Notice::Unlocked;
      }
      if (!nowActive && wasActive && !wasLocked) {
        return Notice::Aborted;
      }
      return Notice::None;
    }

    Out owner(bool isOwned) {
      Out out;
      owned = isOwned;
      if (isOwned) {
        seen = true;
        out.readState = true;
      }
      if (!resolved && !isOwned) {
        resolved = out.resolvedNow = true;
      }
      return out;
    }

    Out stateRead(bool ok, bool propActive, bool propLocked) {
      Out out;
      if (ok) {
        if (propLocked) {
          out.notice = applyState(true, true);
        } else {
          active = propActive;
        }
        if (requested && !propActive) {
          out.sendLock = lock(reason).sendLock;
        }
      }
      if (!resolved) {
        resolved = out.resolvedNow = true;
      }
      return out;
    }

    Out changed(const std::string& state) {
      Out out;
      if (state == "locked") {
        out.notice = applyState(true, true);
      } else if (state == "unlocked" || !locked) {
        out.notice = applyState(false, false);
      } else {
        requested = false;
      }
      return out;
    }

    Out lock(const std::string& asked) {
      if (!requested || asked == "phone") {
        reason = asked;
      }
      requested = true;
      Out out;
      out.sendLock = asked;
      return out;
    }

    Out refused() {
      if (locked) {
        return {};
      }
      Out out;
      out.notice = applyState(false, false);
      return out;
    }
  };

  void readUnlockedKeepsAHeldLock() {
    State state;
    state.apply(State::OwnerChanged{.owned = true});
    state.apply(State::StateChanged{.state = "locked"});
    const auto read = state.apply(State::StateRead{.ok = true, .active = false, .locked = false});
    TEST_CHECK(read.notice == Notice::None && state.locked() && state.active());
    TEST_CHECK(state.apply(State::StateChanged{.state = "unlocked"}).notice == Notice::Unlocked && !state.active());
  }

  void randomSequencesMatchTheFlags() {
    const std::array<const char*, 4> reasons{"user", "phone", "sleep", "relock"};
    const std::array<const char*, 3> states{"locked", "unlocked", "aborted"};
    for (std::uint32_t seed = 1; seed <= 4000; ++seed) {
      std::mt19937 rng(seed);
      State state;
      FlagModel model;
      for (int i = 0; i < 96; ++i) {
        State::Result result;
        FlagModel::Out expect;
        bool deliverable = true;
        switch (rng() % 5) {
        case 0: {
          const bool owned = (rng() & 1U) != 0;
          // NameHasOwner answers only before resolution; NameOwnerChanged any time.
          deliverable = !model.resolved || (rng() & 1U) != 0;
          if (deliverable) {
            result = state.apply(State::OwnerChanged{.owned = owned});
            expect = model.owner(owned);
          }
          break;
        }
        case 1: {
          const bool ok = rng() % 4 != 0;
          const bool active = (rng() & 1U) != 0;
          const bool locked = (rng() & 1U) != 0;
          // A GetAll reply follows a read the process was asked for.
          deliverable = model.seen;
          if (deliverable) {
            result = state.apply(State::StateRead{.ok = ok, .active = active, .locked = locked});
            expect = model.stateRead(ok, active, locked);
          }
          break;
        }
        case 2: {
          const std::string changed = states[rng() % states.size()];
          result = state.apply(State::StateChanged{.state = changed});
          expect = model.changed(changed);
          break;
        }
        case 3: {
          const std::string reason = reasons[rng() % reasons.size()];
          result = state.apply(State::LockAsked{.reason = reason});
          expect = model.lock(reason);
          break;
        }
        default:
          result = state.apply(State::LockRefused{});
          expect = model.refused();
          break;
        }
        if (!deliverable) {
          continue;
        }
        const bool matches = result.notice == expect.notice && result.resolved == expect.resolvedNow
            && result.readState == expect.readState && result.sendLock == expect.sendLock
            && state.resolved() == model.resolved && state.owned() == model.owned && state.seen() == model.seen
            && state.active() == (model.requested || model.active || model.locked) && state.locked() == model.locked
            && state.requested() == model.requested && (!model.requested || state.requestReason() == model.reason);
        if (!matches) {
          TEST_CHECK(false && "lock client state diverged from the flag model");
          return;
        }
      }
    }
  }

} // namespace

int main() {
  readUnlockedKeepsAHeldLock();
  randomSequencesMatchTheFlags();
  return 0;
}
