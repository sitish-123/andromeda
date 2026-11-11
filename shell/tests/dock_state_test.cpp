#include "shell/dock/dock_state.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>
#include <random>

namespace {

  using shell::dock::DockVisibility;
  using shell::dock::DragEvent;
  using shell::dock::DragPhase;
  using shell::dock::dragTransition;
  using Mode = DockVisibility::Mode;

  void swipeReleaseLandsOnTheOverviewState() {
    DockVisibility visibility;
    TEST_CHECK(visibility.apply(DockVisibility::SlideStarted{.anim = 7, .target = 0.0F}).accepted);
    const auto began = visibility.apply(DockVisibility::FollowBegan{.rest = 0.4F});
    TEST_CHECK(began.accepted && began.cancel == std::optional<DockVisibility::AnimId>{7});
    TEST_CHECK(!visibility.apply(DockVisibility::SlideStarted{.anim = 8, .target = 1.0F}).accepted);
    TEST_CHECK(visibility.followRest() == std::optional<float>{0.4F} && !visibility.aside());
    TEST_CHECK(visibility.apply(DockVisibility::OverviewSet{.aside = false}).accepted);
    TEST_CHECK(visibility.mode() == Mode::Settled && !visibility.slideAnim().has_value());
  }

  void asideKeepsItsSlide() {
    DockVisibility visibility;
    TEST_CHECK(visibility.apply(DockVisibility::OverviewSet{.aside = true}).accepted);
    TEST_CHECK(visibility.apply(DockVisibility::SlideStarted{.anim = 3, .target = 0.0F}).accepted);
    TEST_CHECK(!visibility.apply(DockVisibility::OverviewSet{.aside = true}).accepted);
    TEST_CHECK(visibility.apply(DockVisibility::OverviewSet{.aside = false}).accepted);
    TEST_CHECK(visibility.mode() == Mode::Hiding && visibility.slideAnim() == std::optional<DockVisibility::AnimId>{3});
  }

  // The model keeps the overview relation and the slide as plain fields.
  struct VisibilityModel {
    enum class Overview : std::uint8_t { Normal, Aside, Following } overview = Overview::Normal;
    std::optional<DockVisibility::AnimId> anim;
    float target = 0.0F;
    float rest = 0.0F;
    bool fromAside = false;

    [[nodiscard]] Mode mode() const {
      switch (overview) {
      case Overview::Following:
        return Mode::Following;
      case Overview::Aside:
        return Mode::Aside;
      case Overview::Normal:
        break;
      }
      if (!anim.has_value()) {
        return Mode::Settled;
      }
      return target > 0.5F ? Mode::Revealing : Mode::Hiding;
    }
  };

  void randomVisibilitySequencesKeepTheModel() {
    for (std::uint32_t seed = 1; seed <= 4000; ++seed) {
      std::mt19937 rng(seed);
      DockVisibility visibility;
      VisibilityModel model;
      DockVisibility::AnimId nextAnim = 1;
      for (int i = 0; i < 96; ++i) {
        DockVisibility::Result result;
        bool expectAccepted = false;
        std::optional<DockVisibility::AnimId> expectCancel;
        VisibilityModel next = model;
        const bool following = model.overview == VisibilityModel::Overview::Following;
        switch (rng() % 7) {
        case 0:
        case 1: {
          // Show / hide on hover, smart auto-hide or a released overview: a running spring keeps its id.
          const float target = (rng() & 1U) != 0 ? 1.0F : 0.0F;
          const DockVisibility::AnimId anim = model.anim.value_or(nextAnim++);
          result = visibility.apply(DockVisibility::SlideStarted{.anim = anim, .target = target});
          expectAccepted = !following;
          if (expectAccepted) {
            next.anim = anim;
            next.target = target;
          }
          break;
        }
        case 2:
          result = visibility.apply(DockVisibility::SlideEnded{});
          expectAccepted = !following && model.anim.has_value();
          if (expectAccepted) {
            next.anim.reset();
          }
          break;
        case 3:
        case 4: {
          const bool aside = (rng() & 1U) != 0;
          result = visibility.apply(DockVisibility::OverviewSet{.aside = aside});
          const bool isAside = model.overview == VisibilityModel::Overview::Aside;
          expectAccepted = following || isAside != aside;
          if (expectAccepted) {
            next.overview = aside ? VisibilityModel::Overview::Aside : VisibilityModel::Overview::Normal;
            if (following) {
              next.anim.reset();
            }
          }
          break;
        }
        default: {
          const float rest = static_cast<float>(rng() % 5) / 4.0F;
          result = visibility.apply(DockVisibility::FollowBegan{.rest = rest});
          expectAccepted = !following;
          if (expectAccepted) {
            expectCancel = model.anim;
            next.fromAside = model.overview == VisibilityModel::Overview::Aside;
            next.overview = VisibilityModel::Overview::Following;
            next.rest = rest;
            next.anim.reset();
          }
          break;
        }
        }
        if (!expectAccepted) {
          next = model;
        }
        model = next;
        const bool nowFollowing = model.overview == VisibilityModel::Overview::Following;
        const bool expectAside =
            nowFollowing ? model.fromAside : model.overview == VisibilityModel::Overview::Aside;
        if (result.accepted != expectAccepted || result.cancel != expectCancel || visibility.mode() != model.mode()
            || visibility.following() != nowFollowing || visibility.aside() != expectAside
            || visibility.slideAnim() != model.anim
            || visibility.followRest() != (nowFollowing ? std::optional<float>{model.rest} : std::nullopt)) {
          TEST_CHECK(false && "dock visibility diverged from the model");
          return;
        }
        TEST_CHECK(!(visibility.following() && visibility.slideAnim().has_value()));
      }
    }
  }

  void releaseBeforeHoldReturnsToIdle() {
    DragPhase phase = DragPhase::Idle;
    phase = dragTransition(phase, DragEvent::Press).value_or(phase);
    TEST_CHECK(dragTransition(phase, DragEvent::Release) == std::optional{DragPhase::Idle});
    TEST_CHECK(!dragTransition(DragPhase::Armed, DragEvent::Release).has_value());
  }

  void releasedSwipeUpdatesTheOverviewRelation() {
    TEST_CHECK(shell::dock::overviewRelationChanged(0.0F, false, false));
    TEST_CHECK(shell::dock::overviewRelationChanged(std::nullopt, true, false));
    TEST_CHECK(!shell::dock::overviewRelationChanged(std::nullopt, true, true));
  }

  void randomDragSequencesKeepTheModel() {
    for (std::uint32_t seed = 1; seed <= 4000; ++seed) {
      std::mt19937 rng(seed);
      DragPhase phase = DragPhase::Idle;
      bool held = false;
      bool armed = false;
      bool active = false;
      for (int i = 0; i < 64; ++i) {
        const auto event = static_cast<DragEvent>(rng() % 6);
        const std::optional<DragPhase> next = dragTransition(phase, event);
        bool expectAccepted = true;
        switch (event) {
        case DragEvent::Press:
          held = true;
          armed = active = false;
          break;
        case DragEvent::HoldElapsed:
          expectAccepted = held && !armed && !active;
          armed = armed || expectAccepted;
          break;
        case DragEvent::Begin:
          expectAccepted = armed;
          active = active || expectAccepted;
          armed = armed && !expectAccepted;
          break;
        case DragEvent::End:
          expectAccepted = armed || active;
          if (expectAccepted) {
            held = armed = active = false;
          }
          break;
        case DragEvent::Reset:
          held = armed = active = false;
          break;
        case DragEvent::Release:
          expectAccepted = held && !armed && !active;
          held = held && !expectAccepted;
          break;
        }
        if (next.has_value() != expectAccepted) {
          TEST_CHECK(false && "drag transition diverged from the model");
          return;
        }
        if (next.has_value()) {
          phase = *next;
        }
        const DragPhase expect = active ? DragPhase::Active
                                 : armed ? DragPhase::Armed
                                 : held  ? DragPhase::Holding
                                         : DragPhase::Idle;
        if (phase != expect) {
          TEST_CHECK(false && "drag phase diverged from the model");
          return;
        }
      }
    }
  }

} // namespace

int main() {
  swipeReleaseLandsOnTheOverviewState();
  asideKeepsItsSlide();
  randomVisibilitySequencesKeepTheModel();
  releaseBeforeHoldReturnsToIdle();
  releasedSwipeUpdatesTheOverviewRelation();
  randomDragSequencesKeepTheModel();
  return 0;
}
