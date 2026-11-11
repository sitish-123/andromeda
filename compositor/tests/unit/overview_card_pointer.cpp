#include "check.h"
#include "overview/card_pointer.h"

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

using namespace andromeda;

namespace {

  struct FakeCard : Trackable {
    int id = 0;
  };

  // The expected hold, by card id: 0 idle, 1 pressed, 2 dragging.
  struct Model {
    int mode = 0;
    int card = -1;
  };

} // namespace

ANDROMEDA_TEST(randomEventsNeverLeaveAHoldOnADestroyedCard) {
  for (unsigned seed = 1; seed <= 200; ++seed) {
    std::mt19937 rng(seed);
    std::vector<std::unique_ptr<FakeCard>> cards;
    ov::CardPointer<FakeCard> pointer;
    Model model;
    int nextId = 0;
    const auto pick = [&]() -> FakeCard* {
      if (cards.empty() || rng() % 5 == 0) {
        return nullptr;
      }
      return cards[rng() % cards.size()].get();
    };
    for (int step = 0; step < 400; ++step) {
      switch (rng() % 5) {
        case 0: {
          auto card = std::make_unique<FakeCard>();
          card->id = nextId++;
          cards.push_back(std::move(card));
          break;
        }
        case 1: {
          if (cards.empty()) {
            break;
          }
          const size_t index = rng() % cards.size();
          const bool wasDragged = model.mode == 2 && model.card == cards[index]->id;
          CHECK_EQ(pointer.forget(*cards[index]), wasDragged);
          if (model.card == cards[index]->id) {
            model = {};
          }
          cards.erase(cards.begin() + static_cast<std::ptrdiff_t>(index));
          break;
        }
        case 2: {
          FakeCard* card = pick();
          CHECK_EQ(pointer.press(card), model.mode != 2);
          if (model.mode != 2) {
            model = card != nullptr ? Model{1, card->id} : Model{};
          }
          break;
        }
        case 3: {
          FakeCard* dragged = pointer.beginDrag();
          CHECK_EQ(dragged != nullptr, model.mode == 1);
          if (model.mode == 1) {
            model.mode = 2;
          }
          break;
        }
        default:
          pointer.release();
          model = {};
          break;
      }
      const FakeCard* held = pointer.pressed() != nullptr ? pointer.pressed() : pointer.dragged();
      CHECK(pointer.pressed() == nullptr || pointer.dragged() == nullptr);
      CHECK_EQ(pointer.pressed() != nullptr, model.mode == 1);
      CHECK_EQ(pointer.dragged() != nullptr, model.mode == 2);
      CHECK_EQ(held != nullptr ? held->id : -1, model.card);
      CHECK(held == nullptr || std::ranges::any_of(cards, [held](const auto& card) { return card.get() == held; }));
    }
  }
}

int main() { return RUN_TESTS(); }
