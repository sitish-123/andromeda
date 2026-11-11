#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace shell::dock {

  // How the dock stands toward the overview and its auto-hide slide. A finger-driven overview swipe owns the slide
  // value, so no spring runs while Following.
  class DockVisibility {
  public:
    using AnimId = std::uint32_t;

    enum class Mode : std::uint8_t { Settled, Revealing, Hiding, Aside, Following };

    struct SlideStarted {
      AnimId anim = 0;
      float target = 0.0F;
    };
    struct SlideEnded {};
    struct OverviewSet {
      bool aside = false;
    };
    // `rest` is the slide value the dock shows with the overview closed.
    struct FollowBegan {
      float rest = 0.0F;
    };
    using Event = std::variant<SlideStarted, SlideEnded, OverviewSet, FollowBegan>;

    struct Result {
      bool accepted = false;
      // A spring the caller must cancel.
      std::optional<AnimId> cancel;
    };

    [[nodiscard]] Mode mode() const noexcept {
      if (std::holds_alternative<Following>(m_state)) {
        return Mode::Following;
      }
      if (std::holds_alternative<Aside>(m_state)) {
        return Mode::Aside;
      }
      const auto* sliding = std::get_if<Sliding>(&std::get<Normal>(m_state).slide);
      if (sliding == nullptr) {
        return Mode::Settled;
      }
      return sliding->target > 0.5F ? Mode::Revealing : Mode::Hiding;
    }
    [[nodiscard]] bool following() const noexcept { return std::holds_alternative<Following>(m_state); }
    // While Following, the overview state the swipe started from.
    [[nodiscard]] bool aside() const noexcept {
      if (const auto* follow = std::get_if<Following>(&m_state)) {
        return follow->fromAside;
      }
      return std::holds_alternative<Aside>(m_state);
    }
    [[nodiscard]] std::optional<float> followRest() const noexcept {
      if (const auto* follow = std::get_if<Following>(&m_state)) {
        return follow->rest;
      }
      return std::nullopt;
    }
    [[nodiscard]] std::optional<AnimId> slideAnim() const noexcept {
      const Slide* slide = slideOf();
      if (slide == nullptr) {
        return std::nullopt;
      }
      if (const auto* sliding = std::get_if<Sliding>(slide)) {
        return sliding->anim;
      }
      return std::nullopt;
    }

    Result apply(Event event) {
      return std::visit([this](auto&& e) { return on(std::forward<decltype(e)>(e)); }, std::move(event));
    }

  private:
    struct Settled {};
    struct Sliding {
      AnimId anim = 0;
      float target = 0.0F;
    };
    using Slide = std::variant<Settled, Sliding>;
    struct Normal {
      Slide slide;
    };
    struct Aside {
      Slide slide;
    };
    struct Following {
      float rest = 0.0F;
      bool fromAside = false;
    };

    Slide* slideOf() noexcept {
      if (auto* normal = std::get_if<Normal>(&m_state)) {
        return &normal->slide;
      }
      if (auto* aside = std::get_if<Aside>(&m_state)) {
        return &aside->slide;
      }
      return nullptr;
    }
    [[nodiscard]] const Slide* slideOf() const noexcept { return const_cast<DockVisibility*>(this)->slideOf(); }

    Result on(SlideStarted event) {
      Slide* slide = slideOf();
      if (slide == nullptr) {
        return {};
      }
      *slide = Sliding{.anim = event.anim, .target = event.target};
      return {.accepted = true, .cancel = {}};
    }

    Result on(SlideEnded /*event*/) {
      Slide* slide = slideOf();
      if (slide == nullptr || !std::holds_alternative<Sliding>(*slide)) {
        return {};
      }
      *slide = Settled{};
      return {.accepted = true, .cancel = {}};
    }

    Result on(OverviewSet event) {
      if (following()) {
        m_state = event.aside ? State{Aside{}} : State{Normal{}};
        return {.accepted = true, .cancel = {}};
      }
      if (aside() == event.aside) {
        return {};
      }
      Slide slide = *slideOf();
      m_state = event.aside ? State{Aside{.slide = slide}} : State{Normal{.slide = slide}};
      return {.accepted = true, .cancel = {}};
    }

    Result on(FollowBegan event) {
      if (following()) {
        return {};
      }
      Result result{.accepted = true, .cancel = slideAnim()};
      m_state = Following{.rest = event.rest, .fromAside = aside()};
      return result;
    }

    using State = std::variant<Normal, Aside, Following>;
    State m_state = Normal{};
  };

  // A released swipe (`releasedRest` set) or a changed overview state must update the dock's overview relation.
  [[nodiscard]] constexpr bool overviewRelationChanged(std::optional<float> releasedRest, bool overviewAside,
                                                       bool dockAside) noexcept {
    return releasedRest.has_value() || overviewAside != dockAside;
  }

  enum class DragPhase : std::uint8_t { Idle, Holding, Armed, Active };

  // Release: the button comes up before the hold delay, so the press stays a click.
  enum class DragEvent : std::uint8_t { Press, HoldElapsed, Begin, End, Reset, Release };

  // The phase after `event`, or nullopt when `event` does not apply in `from`.
  [[nodiscard]] constexpr std::optional<DragPhase> dragTransition(DragPhase from, DragEvent event) noexcept {
    switch (event) {
    case DragEvent::Press:
      return DragPhase::Holding;
    case DragEvent::HoldElapsed:
      return from == DragPhase::Holding ? std::optional{DragPhase::Armed} : std::nullopt;
    case DragEvent::Begin:
      return from == DragPhase::Armed ? std::optional{DragPhase::Active} : std::nullopt;
    case DragEvent::End:
      return from == DragPhase::Armed || from == DragPhase::Active ? std::optional{DragPhase::Idle} : std::nullopt;
    case DragEvent::Reset:
      return DragPhase::Idle;
    case DragEvent::Release:
      return from == DragPhase::Holding ? std::optional{DragPhase::Idle} : std::nullopt;
    }
    return std::nullopt;
  }

} // namespace shell::dock
