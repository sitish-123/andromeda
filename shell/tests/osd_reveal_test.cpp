#include "shell/osd/osd_reveal.h"
#include "tests/test_check.h"

#include <cstdint>
#include <random>

namespace {

  using State = OsdReveal::State;
  using Cmd = OsdReveal::Cmd;

  enum class Anim : std::uint8_t { None, ShowSpring, HoldTimer, HideSpring };

  // The one animation slot of the overlay instance: completions arrive only for the running animation.
  struct FakeSlot {
    Anim running = Anim::None;
    void apply(Cmd cmd) {
      switch (cmd) {
      case Cmd::Keep:
        break;
      case Cmd::Stop:
        running = Anim::None;
        break;
      case Cmd::Show:
        running = Anim::ShowSpring;
        break;
      case Cmd::Hold:
        running = Anim::HoldTimer;
        break;
      case Cmd::Hide:
        running = Anim::HideSpring;
        break;
      }
    }
  };

  Anim expectedAnim(State state) {
    switch (state) {
    case State::Showing:
      return Anim::ShowSpring;
    case State::Holding:
      return Anim::HoldTimer;
    case State::Hiding:
      return Anim::HideSpring;
    case State::Hidden:
    case State::ShowPending:
      return Anim::None;
    }
    return Anim::None;
  }

  bool invariantsHold(const OsdReveal& reveal, const FakeSlot& slot) {
    if (slot.running != expectedAnim(reveal.state())) {
      return false;
    }
    if (reveal.state() == State::Hidden && reveal.reveal() != 0.0F) {
      return false;
    }
    // A held card is fully revealed: a show request never snaps a half-hidden card back.
    return reveal.state() != State::Holding || reveal.reveal() == 1.0F;
  }

  void testRandomSequencesKeepInvariants() {
    std::mt19937 rng(0x05d1);
    std::uniform_real_distribution<float> frac(0.0F, 1.0F);
    for (int run = 0; run < 2000; ++run) {
      OsdReveal reveal;
      FakeSlot slot;
      for (int step = 0; step < 80; ++step) {
        switch (std::uniform_int_distribution<int>(0, 4)(rng)) {
        case 0: {
          const State before = reveal.state();
          slot.apply(reveal.request());
          TEST_CHECK(reveal.state() == State::ShowPending || before == State::Showing);
          break;
        }
        case 1:
          slot.apply(reveal.frame());
          break;
        case 2:
          if (slot.running == Anim::ShowSpring || slot.running == Anim::HideSpring) {
            reveal.setReveal(frac(rng));
          }
          break;
        case 3:
          if (slot.running == Anim::ShowSpring) {
            reveal.setReveal(1.0F);
            slot.running = Anim::None;
            slot.apply(reveal.showSettled());
          } else if (slot.running == Anim::HoldTimer) {
            slot.running = Anim::None;
            slot.apply(reveal.holdElapsed());
          } else if (slot.running == Anim::HideSpring) {
            reveal.setReveal(0.0F);
            slot.running = Anim::None;
            slot.apply(reveal.hideSettled());
          }
          break;
        default:
          break;
        }
        TEST_CHECK(invariantsHold(reveal, slot));
      }
      // Without new requests, frames and completions always bring the card back to Hidden.
      for (int drain = 0; drain < 5 && reveal.state() != State::Hidden; ++drain) {
        slot.apply(reveal.frame());
        if (slot.running == Anim::ShowSpring) {
          reveal.setReveal(1.0F);
          slot.running = Anim::None;
          slot.apply(reveal.showSettled());
        } else if (slot.running == Anim::HoldTimer) {
          slot.running = Anim::None;
          slot.apply(reveal.holdElapsed());
        } else if (slot.running == Anim::HideSpring) {
          reveal.setReveal(0.0F);
          slot.running = Anim::None;
          slot.apply(reveal.hideSettled());
        }
        TEST_CHECK(invariantsHold(reveal, slot));
      }
      TEST_CHECK(reveal.state() == State::Hidden);
    }
  }

} // namespace

int main() {
  testRandomSequencesKeepInvariants();
  return 0;
}
