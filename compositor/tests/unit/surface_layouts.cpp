#include "input/surface_layouts.h"

#include "check.h"
#include "wlr.h"

namespace {

  struct TestSurface {
    TestSurface() { wl_signal_init(&surface.events.destroy); }

    wlr_surface surface{};
  };

} // namespace

UMBRIEL_TEST(remembersNamedLayoutsPerSurface) {
  umbriel::SurfaceLayoutMemory memory;
  TestSurface first;
  TestSurface second;

  CHECK(!memory.recall(&first.surface).has_value());
  memory.remember(&first.surface, "English (US)");
  memory.remember(&second.surface, "German");

  CHECK_EQ(memory.size(), size_t{2});
  CHECK_EQ(memory.recall(&first.surface), std::optional<std::string_view>{"English (US)"});
  CHECK_EQ(memory.recall(&second.surface), std::optional<std::string_view>{"German"});

  memory.remember(&first.surface, "Persian");
  CHECK_EQ(memory.recall(&first.surface), std::optional<std::string_view>{"Persian"});
}

UMBRIEL_TEST(forgetsDestroyedSurfaces) {
  umbriel::SurfaceLayoutMemory memory;
  TestSurface first;
  TestSurface second;
  memory.remember(&first.surface, "English (US)");
  memory.remember(&second.surface, "German");

  wl_signal_emit_mutable(&first.surface.events.destroy, &first.surface);

  CHECK_EQ(memory.size(), size_t{1});
  CHECK(!memory.recall(&first.surface).has_value());
  CHECK_EQ(memory.recall(&second.surface), std::optional<std::string_view>{"German"});

  memory.clear();
  CHECK_EQ(memory.size(), size_t{0});
  CHECK(wl_list_empty(&second.surface.events.destroy.listener_list));
}

int main() { return RUN_TESTS(); }
