#pragma once

#include "util/tracked.h"

#include <cstdint>
#include <optional>
#include <variant>

namespace andromeda::ov {

  // A device that holds the overview's left button: the pointer, or one touch point.
  struct HoldSource {
    enum class Kind : uint8_t { Pointer, Touch };
    Kind kind = Kind::Pointer;
    int32_t touchId = 0;
    static constexpr HoldSource pointer() { return {}; }
    static constexpr HoldSource touch(int32_t id) { return {.kind = Kind::Touch, .touchId = id}; }
    bool operator==(const HoldSource&) const = default;
  };

  // What a press landed on: a card's body (it can drag) or a card's close button (a release on it closes the window).
  enum class PressTarget : uint8_t { Card, CloseButton };

  // The left button's hold in the overview: idle, pressed (on a card, its close button or nothing), or dragging a card.
  // One device owns
  // a hold from press to release; the other devices' presses are rejected until then. Card must derive from
  // Trackable. The owner calls forget() before it destroys a card, so the hold ends with the card and the owner can
  // undo what the drag set up.
  template <class Card> class CardPointer {
  public:
    [[nodiscard]] Card* pressed() const {
      const auto* state = std::get_if<Pressed>(&m_state);
      return state != nullptr && state->target == PressTarget::Card ? state->card.get() : nullptr;
    }
    // The card whose close button is held, null for any other hold.
    [[nodiscard]] Card* closePressed() const {
      const auto* state = std::get_if<Pressed>(&m_state);
      return state != nullptr && state->target == PressTarget::CloseButton ? state->card.get() : nullptr;
    }
    [[nodiscard]] Card* dragged() const {
      const auto* state = std::get_if<Dragging>(&m_state);
      return state != nullptr ? state->card.get() : nullptr;
    }
    [[nodiscard]] std::optional<HoldSource> owner() const {
      if (const auto* state = std::get_if<Pressed>(&m_state)) {
        return state->owner;
      }
      if (const auto* state = std::get_if<Dragging>(&m_state)) {
        return state->owner;
      }
      return std::nullopt;
    }
    // Whether input from `from` may act on the hold: nothing is held, or `from` holds it.
    [[nodiscard]] bool admits(HoldSource from) const {
      const std::optional<HoldSource> holder = owner();
      return !holder.has_value() || *holder == from;
    }

    // Idle or Pressed by `from` -> Pressed by `from` on `card`'s `target` (null card: on nothing). Rejected while
    // dragging and while another device holds.
    bool press(HoldSource from, Card* card, PressTarget target = PressTarget::Card) {
      if (std::holds_alternative<Dragging>(m_state) || !admits(from)) {
        return false;
      }
      m_state = Pressed{.owner = from, .card = card, .target = target};
      return true;
    }

    // Pressed on a card's body -> Dragging it, same owner. Returns the dragged card, or null (and no change) otherwise.
    Card* beginDrag() {
      auto* state = std::get_if<Pressed>(&m_state);
      Card* card = pressed();
      if (card != nullptr) {
        m_state = Dragging{.owner = state->owner, .card = card};
      }
      return card;
    }

    void release() { m_state = Idle{}; }

    // Ends any hold on `card`. Returns true when that ended a drag.
    bool forget(const Card& card) {
      const bool wasDragging = dragged() == &card;
      if (wasDragging || pressed() == &card || closePressed() == &card) {
        m_state = Idle{};
      }
      return wasDragging;
    }

  private:
    struct Idle {};
    struct Pressed {
      HoldSource owner;
      Ref<Card> card;
      PressTarget target = PressTarget::Card;
    };
    struct Dragging {
      HoldSource owner;
      Ref<Card> card;
    };
    using State = std::variant<Idle, Pressed, Dragging>;
    State m_state;
  };

} // namespace andromeda::ov
