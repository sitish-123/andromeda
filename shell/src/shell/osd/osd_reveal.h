#pragma once

#include <algorithm>
#include <cstdint>

// The reveal of one OSD card. A show request during Holding or Hiding goes back to ShowPending, and the next frame
// springs from the current reveal: a new value reverses a hide instead of popping. The hold timer starts only at full
// reveal, so a short hide delay cannot end a show spring. At most one animation runs: the one of the current state.
class OsdReveal {
public:
  enum class State : std::uint8_t { Hidden, ShowPending, Showing, Holding, Hiding };
  // What the caller does with its one animation slot. Show, Hold and Hide replace the running animation;
  // Show and Hide spring from reveal().
  enum class Cmd : std::uint8_t { Keep, Stop, Show, Hold, Hide };

  [[nodiscard]] State state() const noexcept { return m_state; }
  [[nodiscard]] float reveal() const noexcept { return m_reveal; }
  // On screen or animating; Hiding counts, so a correction during the hide shows the card again.
  [[nodiscard]] bool visible() const noexcept { return m_state != State::Hidden; }

  Cmd request() {
    switch (m_state) {
    case State::Hidden:
      return to(State::ShowPending, Cmd::Keep);
    case State::Holding:
    case State::Hiding:
      return to(State::ShowPending, Cmd::Stop);
    case State::ShowPending:
    case State::Showing:
      return Cmd::Keep;
    }
    return Cmd::Keep;
  }
  // The surface prepares a frame with a built scene.
  Cmd frame() {
    if (m_state != State::ShowPending) {
      return Cmd::Keep;
    }
    return m_reveal >= 1.0F ? to(State::Holding, Cmd::Hold) : to(State::Showing, Cmd::Show);
  }
  void setReveal(float reveal) noexcept { m_reveal = std::clamp(reveal, 0.0F, 1.0F); }
  Cmd showSettled() {
    if (m_state != State::Showing) {
      return Cmd::Keep;
    }
    m_reveal = 1.0F;
    return to(State::Holding, Cmd::Hold);
  }
  Cmd holdElapsed() { return m_state == State::Holding ? to(State::Hiding, Cmd::Hide) : Cmd::Keep; }
  Cmd hideSettled() {
    if (m_state != State::Hiding) {
      return Cmd::Keep;
    }
    m_reveal = 0.0F;
    return to(State::Hidden, Cmd::Keep);
  }

private:
  Cmd to(State next, Cmd cmd) noexcept {
    m_state = next;
    return cmd;
  }

  State m_state = State::Hidden;
  float m_reveal = 0.0F;
};
