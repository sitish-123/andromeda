#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "shell/bar/lens_track.h"
#include "shell/bar/widgets/workspaces_widget.h"
#include "tests/test_check.h"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

class WorkspacesWidgetTestAccess {
public:
  static std::string tooltip(std::string name, const std::string& label, bool showLabel) {
    return WorkspacesWidget::workspaceTooltipText(Workspace{.name = std::move(name)}, label, showLabel);
  }

  // Island dots: 7 px dots, the current one a 20 px capsule, 5 px gaps (rulebook 9.1).
  static std::pair<float, float> slideLens(std::size_t count, std::size_t from, std::size_t to, double progress) {
    std::vector<WorkspacesWidget::Item> items(count);
    for (auto& item : items) {
      item.inactiveWidth = 7.0F;
      item.activeWidth = 20.0F;
    }
    const auto span = WorkspacesWidget::layoutSlide(items, from, to, progress, 5.0F);
    return {span.start, span.end};
  }
};

namespace {
  bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }
} // namespace

int main() {
  // A name cut short by max_label_chars is recoverable on hover.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("client portal", "cli", true) == "client portal");
  // A pill that already spells out the whole name has nothing to add.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("web", "web", true).empty());
  // label_source = "id" labels a named workspace by number, so the name is only reachable on hover.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("client portal", "3", true) == "client portal");
  // A hidden label shows nothing, even when its text would have matched the name.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("web", "web", false) == "web");
  // Unnamed workspaces have no tooltip, never an empty one.
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("", "3", true).empty());
  TEST_CHECK(WorkspacesWidgetTestAccess::tooltip("", "", false).empty());

  // The capsule sits on the compositor's slide position: at rest on the source dot, on the target dot at the end,
  // and the same fraction of the way between them in the middle (dots 0 7 | 12 32 | 37 44 | 49 56 at rest on 1).
  const auto atSource = WorkspacesWidgetTestAccess::slideLens(4, 1, 2, 0.0);
  TEST_CHECK(near(atSource.first, 12.0F) && near(atSource.second, 32.0F));
  const auto atTarget = WorkspacesWidgetTestAccess::slideLens(4, 1, 2, 1.0);
  TEST_CHECK(near(atTarget.first, 24.0F) && near(atTarget.second, 44.0F));
  // 40 %: the capsule keeps its 20 px and is 40 % of the way from 12 to 24.
  const auto between = WorkspacesWidgetTestAccess::slideLens(4, 1, 2, 0.4);
  TEST_CHECK(near(between.first, 16.8F) && near(between.second, 36.8F));
  // A jump from 0 to 3 passes the dots between: half way, it sits half way between dots 1 and 2.
  const auto jump = WorkspacesWidgetTestAccess::slideLens(4, 0, 3, 0.5);
  const auto jumpRef = WorkspacesWidgetTestAccess::slideLens(4, 1, 2, 0.5);
  TEST_CHECK(near(jump.first, jumpRef.first) && near(jump.second, jumpRef.second));
  // A swipe back toward a lower workspace mirrors the forward slide.
  const auto back = WorkspacesWidgetTestAccess::slideLens(4, 2, 1, 0.6);
  TEST_CHECK(near(back.first, between.first) && near(back.second, between.second));

  // While it follows the slide the lens has no spring of its own: a travel already running stops where the slide is.
  {
    Node parent;
    AnimationManager animations;
    LensTrack lens;
    lens.setFlat(colorSpecFromRole(ColorRole::Accent));
    lens.attach(parent, animations, false, 1.0F);
    lens.moveTo(12.0F, 32.0F, 0.0F, 7.0F, 3.5F, false);
    lens.moveTo(24.0F, 44.0F, 0.0F, 7.0F, 3.5F, true);
    animations.tick(16.0F);
    lens.follow(16.8F, 36.8F, 0.0F, 7.0F, 3.5F);
    animations.tick(16.0F);
    animations.tick(16.0F);
    TEST_CHECK(near(lens.start(), 16.8F) && near(lens.end(), 36.8F));
  }

  // Reduced motion: the lens jumps with the dots each frame of their travel, and its fade-in keeps rising.
  {
    MotionService::instance().setReduced(true);
    Node parent;
    AnimationManager animations;
    LensTrack lens;
    lens.setFlat(colorSpecFromRole(ColorRole::Accent));
    lens.attach(parent, animations, false, 1.0F);
    lens.moveTo(12.0F, 32.0F, 0.0F, 7.0F, 3.5F, false);
    lens.moveTo(14.0F, 34.0F, 0.0F, 7.0F, 3.5F, true);
    animations.tick(16.0F);
    const float first = lens.opacity();
    lens.moveTo(16.0F, 36.0F, 0.0F, 7.0F, 3.5F, true);
    animations.tick(16.0F);
    TEST_CHECK(near(lens.start(), 16.0F));
    TEST_CHECK(first < 1.0F && lens.opacity() > first);
    MotionService::instance().setReduced(false);
  }

  return 0;
}
