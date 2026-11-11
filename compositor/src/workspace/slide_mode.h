#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace andromeda {

  // A workspace slide: Idle, Tracking (a gesture or a carried offset moves it) or Settling (an animation carries it to a
  // step). Tracking and Settling own the slide's data and one Hold (move-only, released when it goes), so the hold ends
  // exactly when the slide does.
  template <class Data, class Hold> class SlideMode {
  public:
    enum class Phase : uint8_t { Idle, Tracking, Settling };

    [[nodiscard]] Phase phase() const {
      if (std::holds_alternative<Tracking>(m_state)) {
        return Phase::Tracking;
      }
      return std::holds_alternative<Settling>(m_state) ? Phase::Settling : Phase::Idle;
    }
    [[nodiscard]] bool active() const { return phase() != Phase::Idle; }
    // The running slide's data; null when idle.
    [[nodiscard]] Data* data() {
      if (auto* state = std::get_if<Tracking>(&m_state)) {
        return &state->data;
      }
      if (auto* state = std::get_if<Settling>(&m_state)) {
        return &state->data;
      }
      return nullptr;
    }
    [[nodiscard]] const Data* data() const { return const_cast<SlideMode*>(this)->data(); }

    // Idle -> Tracking. Rejected in the other phases: the caller finishes the running slide first.
    bool track(Data data, Hold hold) {
      if (active()) {
        return false;
      }
      m_state.template emplace<Tracking>(std::move(data), std::move(hold));
      return true;
    }

    // Tracking -> Settling. Rejected in the other phases.
    bool settle() {
      auto* state = std::get_if<Tracking>(&m_state);
      if (state == nullptr) {
        return false;
      }
      Tracking tracking = std::move(*state);
      m_state.template emplace<Settling>(std::move(tracking.data), std::move(tracking.hold));
      return true;
    }

    // Any phase -> Idle. Returns the data of the slide that ran so the caller can end what it showed; its hold goes.
    std::optional<Data> finish() {
      std::optional<Data> ended;
      if (Data* running = data()) {
        ended.emplace(std::move(*running));
      }
      m_state.template emplace<Idle>();
      return ended;
    }

  private:
    struct Idle {};
    struct Tracking {
      Data data;
      Hold hold;
    };
    struct Settling {
      Data data;
      Hold hold;
    };
    std::variant<Idle, Tracking, Settling> m_state;
  };

} // namespace andromeda
