#include "check.h"
#include "view/snap_assist_mode.h"

#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace andromeda;

namespace {

  std::set<int> live;
  std::vector<std::string> ends;

  struct Part {
    int id = -1;
    const char* kind = "";
    Part(int id, const char* kind) : id(id), kind(kind) {}
    Part(Part&& other) noexcept : id(std::exchange(other.id, -1)), kind(other.kind) {}
    Part& operator=(Part&&) = delete;
    ~Part() {
      if (id >= 0) {
        ends.push_back(std::string(kind) + std::to_string(id));
      }
    }
  };

  // Same member order as SnapAssist::Picture: content first, so the cards end first.
  struct FakePicture {
    Part content;
    Part cards;
    explicit FakePicture(int id) : content(id, "content"), cards(id, "cards") { live.insert(id); }
    FakePicture(FakePicture&& other) noexcept = default;
    FakePicture& operator=(FakePicture&&) = delete;
    ~FakePicture() {
      if (content.id >= 0) {
        live.erase(content.id);
      }
    }
  };

  enum class Mode { Closed, Open, FadingOut };

} // namespace

ANDROMEDA_TEST(randomEventsKeepOnePictureOwnedByTheMode) {
  for (unsigned seed = 1; seed <= 200; ++seed) {
    std::mt19937 rng(seed);
    live.clear();
    ends.clear();
    {
      SnapAssistMode<FakePicture> mode;
      Mode model = Mode::Closed;
      int shown = -1;
      int nextId = 0;
      for (int step = 0; step < 400; ++step) {
        const int previous = shown;
        switch (rng() % 4) {
          case 0:
            mode.show(FakePicture(nextId));
            model = Mode::Open;
            shown = nextId++;
            break;
          case 1:
            CHECK_EQ(mode.fadeOut(), model == Mode::Open);
            if (model == Mode::Open) {
              model = Mode::FadingOut;
            }
            break;
          case 2:
            mode.close();
            model = Mode::Closed;
            shown = -1;
            break;
          default:
            if (mode.fading()) {
              mode.close();
            }
            if (model == Mode::FadingOut) {
              model = Mode::Closed;
              shown = -1;
            }
            break;
        }
        if (previous >= 0 && previous != shown) {
          CHECK(ends.size() >= 2);
          CHECK_EQ(ends[ends.size() - 2], "cards" + std::to_string(previous));
          CHECK_EQ(ends.back(), "content" + std::to_string(previous));
        }
        CHECK_EQ(mode.open(), model == Mode::Open);
        CHECK_EQ(mode.fading(), model == Mode::FadingOut);
        FakePicture* picture = mode.picture();
        if (model == Mode::Closed) {
          CHECK(picture == nullptr);
          CHECK(live.empty());
        } else {
          CHECK(picture != nullptr && picture->content.id == shown && picture->cards.id == shown);
          CHECK(live == std::set<int>{shown});
        }
      }
    }
    CHECK(live.empty());
  }
}

int main() { return RUN_TESTS(); }
