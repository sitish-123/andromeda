#include "check.h"
#include "core/animation.h"
#include "overview/preview_geometry.h"

#include <cstdint>

namespace {
  // A 2560x1600 output at scale 1.5 is 1707x1067 in layout coordinates; the overview previews it at half size.
  constexpr int kOutputWidth = 1707;
  constexpr int kOutputHeight = 1067;
  constexpr double kZoom = 0.5;
  constexpr int kRefreshHz = 165;
} // namespace

ANDROMEDA_TEST(previewGridKeepsFractionalOutputsOnWholePixels) {
  const andromeda::PreviewGrid grid = andromeda::previewGrid(0, 0, kOutputWidth, kOutputHeight, kZoom, false);
  CHECK_EQ(grid.step(false), 587);
  // Whole rows of scroll land every preview on the lattice the grid defines.
  for (size_t index = 0; index < 5; ++index) {
    for (int scroll = 0; scroll < 5; ++scroll) {
      const int origin = andromeda::previewAxisOrigin(grid, false, index, scroll);
      CHECK_EQ(origin, grid.baseY + (static_cast<int>(index) - scroll) * grid.step(false));
    }
  }

  const andromeda::PreviewGrid offset = andromeda::previewGrid(1707, 0, kOutputWidth, kOutputHeight, kZoom, true);
  CHECK_EQ(offset.baseX - 1707, grid.baseX);
  CHECK_EQ(offset.step(true), offset.width + offset.gap);
}

// A filmstrip spring that settles four rows away reaches the target row's pixel and never leaves it again, and every
// frame on the way moves toward it.
ANDROMEDA_TEST(settlingFilmstripReachesAndHoldsItsRowOnFractionalOutputs) {
  const andromeda::PreviewGrid grid = andromeda::previewGrid(0, 0, kOutputWidth, kOutputHeight, kZoom, false);
  const int step = grid.step(false);
  const andromeda::SpringConfig spring{.damping = 1.0, .stiffness = 1000.0, .mass = 1.0};
  constexpr size_t kTargetRow = 4;

  andromeda::AnimatedValue scroll{0.0};
  scroll.settleSpring(static_cast<double>(kTargetRow), spring, 0.0);
  CHECK(scroll.tick(1000));
  int previous = andromeda::previewAxisOrigin(grid, false, kTargetRow, scroll.current());
  bool arrived = false;
  int frames = 0;
  for (int frame = 1; frame < 400 && scroll.animating(); ++frame) {
    scroll.tick(1000 + static_cast<uint64_t>(frame * 1000 / kRefreshHz));
    static_cast<void>(scroll.finishSpringTail(step));
    const int origin = andromeda::previewAxisOrigin(grid, false, kTargetRow, scroll.current());
    CHECK(origin <= previous);
    CHECK(origin >= grid.baseY);
    if (arrived) {
      CHECK_EQ(origin, grid.baseY);
    }
    arrived = arrived || origin == grid.baseY;
    previous = origin;
    frames = frame;
  }
  CHECK(arrived);
  CHECK(!scroll.animating());
  CHECK(frames > 3);
  CHECK_EQ(andromeda::previewAxisOrigin(grid, false, kTargetRow, scroll.current()), grid.baseY);
}

int main() { return RUN_TESTS(); }
