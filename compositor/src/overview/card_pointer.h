#pragma once

#include "util/tracked.h"

#include <variant>

namespace andromeda::ov {

  // The left button's hold on an overview card: idle, pressed on a card, or dragging it. Card must derive from
  // Trackable. The owner calls forget() before it destroys a card, so the hold ends with the card and the owner can
  // undo what the drag set up.
  template <class Card> class CardPointer {
  public:
    [[nodiscard]] Card* pressed() const {
      const auto* state = std::get_if<Pressed>(&m_state);
      return state != nullptr ? state->card.get() : nullptr;
    }
    [[nodiscard]] Card* dragged() const {
      const auto* state = std::get_if<Dragging>(&m_state);
      return state != nullptr ? state->card.get() : nullptr;
    }

    // Idle or Pressed -> Pressed on `card`, or Idle when it is null. Rejected while dragging.
    bool press(Card* card) {
      if (std::holds_alternative<Dragging>(m_state)) {
        return false;
      }
      m_state = card != nullptr ? State{Pressed{card}} : State{Idle{}};
      return true;
    }

    // Pressed -> Dragging. Returns the dragged card, or null (and no change) when no card is pressed.
    Card* beginDrag() {
      Card* card = pressed();
      if (card != nullptr) {
        m_state = Dragging{card};
      }
      return card;
    }

    void release() { m_state = Idle{}; }

    // Ends any hold on `card`. Returns true when that ended a drag.
    bool forget(const Card& card) {
      const bool wasDragging = dragged() == &card;
      if (wasDragging || pressed() == &card) {
        m_state = Idle{};
      }
      return wasDragging;
    }

  private:
    struct Idle {};
    struct Pressed {
      Ref<Card> card;
    };
    struct Dragging {
      Ref<Card> card;
    };
    using State = std::variant<Idle, Pressed, Dragging>;
    State m_state;
  };

} // namespace andromeda::ov
