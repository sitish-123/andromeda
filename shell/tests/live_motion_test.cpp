// MotionService follows the compositor's animation.enabled, animation.speed and appearance.reduce_motion as they change.
#include "render/animation/motion_service.h"
#include "theme/live_appearance_sync.h"

#include <map>
#include <print>
#include <string>
#include <string_view>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "live_motion: FAIL: {}", message);
      ++g_failures;
    }
  }

} // namespace

int main() {
  std::map<std::string, std::string, std::less<>> compositor{
      {"animation.enabled", "false"},
      {"animation.speed", "2.5"},
  };
  const auto sync = [&compositor] {
    applyLiveMotion(readLiveAppearance([&compositor](std::string_view key) {
      const auto it = compositor.find(key);
      return it != compositor.end() ? it->second : std::string();
    }));
  };
  const MotionService& motion = MotionService::instance();

  sync();
  expect(!motion.enabled(), "animation.enabled = false left shell motion on");
  expect(motion.speed() == 2.5F, "shell motion did not take animation.speed");

  compositor["animation.enabled"] = "true";
  compositor["animation.speed"] = "0.5";
  compositor["appearance.reduce_motion"] = "true";
  sync();
  expect(motion.enabled(), "turning animation.enabled on did not reach shell motion");
  expect(motion.speed() == 0.5F, "a changed animation.speed did not reach shell motion");
  expect(motion.reduced(), "appearance.reduce_motion did not reach shell motion");

  if (g_failures != 0) {
    return 1;
  }
  std::println("live_motion: ok");
  return 0;
}
