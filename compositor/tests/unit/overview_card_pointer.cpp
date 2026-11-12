#include "check.h"
#include "overview/card_pointer.h"

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <vector>

using namespace andromeda;

namespace {

  struct FakeCard : Trackable {
    int id = 0;
  };

  // The expected hold, by card id (-1: none): 0 idle, 1 pressed, 2 dragging; `owner` holds it unless idle.
  struct Model {
    int mode = 0;
    int card = -1;
    ov::HoldSource owner;
  };

  constexpr std::array kSources{ov::HoldSource::pointer(), ov::HoldSource::touch(1), ov::HoldSource::touch(2)};

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
          const ov::HoldSource from = kSources[rng() % kSources.size()];
          const bool accepted = model.mode == 0 || (model.mode == 1 && model.owner == from);
          CHECK_EQ(pointer.press(from, card), accepted);
          if (accepted) {
            model = Model{1, card != nullptr ? card->id : -1, from};
          }
          break;
        }
        case 3: {
          FakeCard* dragged = pointer.beginDrag();
          CHECK_EQ(dragged != nullptr, model.mode == 1 && model.card >= 0);
          if (model.mode == 1 && model.card >= 0) {
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
      CHECK_EQ(pointer.pressed() != nullptr, model.mode == 1 && model.card >= 0);
      CHECK(pointer.owner() == (model.mode != 0 ? std::optional(model.owner) : std::nullopt));
      for (const ov::HoldSource& from : kSources) {
        CHECK_EQ(pointer.admits(from), model.mode == 0 || model.owner == from);
      }
      CHECK_EQ(pointer.dragged() != nullptr, model.mode == 2);
      CHECK_EQ(held != nullptr ? held->id : -1, model.card);
      CHECK(held == nullptr || std::ranges::any_of(cards, [held](const auto& card) { return card.get() == held; }));
    }
  }
}

ANDROMEDA_TEST(aDragBelongsToTheDeviceThatPressed) {
  FakeCard held;
  FakeCard other;
  ov::CardPointer<FakeCard> pointer;
  CHECK(pointer.press(ov::HoldSource::touch(4), &held));
  CHECK(pointer.beginDrag() == &held);
  CHECK(!pointer.admits(ov::HoldSource::pointer()));
  CHECK(!pointer.admits(ov::HoldSource::touch(5)));
  CHECK(!pointer.press(ov::HoldSource::pointer(), &other));
  CHECK(pointer.dragged() == &held);
  CHECK(pointer.owner() == ov::HoldSource::touch(4));
  pointer.release();
  CHECK(pointer.press(ov::HoldSource::pointer(), nullptr));
  CHECK(!pointer.press(ov::HoldSource::touch(4), &held));
  CHECK(pointer.pressed() == nullptr);
}

int main() { return RUN_TESTS(); }
