#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

// The face morph of the center island: the shown face, the face that fades out, and the driver that steps the morph.
// Settled: no driver, no outgoing face. AwaitStart: the shape and the fade hold until the compositor's `started` for
// `gen` or the start wait. Running: the driver steps them. A transition that ends a morph returns its driver to cancel.
template <typename Face, typename Rect, typename Driver> class IslandMorph {
public:
  enum class Phase : std::uint8_t { Settled, AwaitStart, Running };

  // A face change on a new non-zero driver; it runs at once unless a Retarget follows.
  struct Begin {
    Face to;
    Rect fromRect; // the outgoing card, taken before the model drops its content
    Driver driver;
  };
  // A face change without motion.
  struct Snap {
    Face to;
  };
  // The compositor got a new clip target: wait for its `started`. `driver` is the running one, or a new one if settled.
  struct Retarget {
    Driver driver;
  };
  struct Started {
    std::uint32_t gen;
  };
  struct StartTimeout {};
  // The mirror springs settled.
  struct Settle {};
  // A driver ended on its own: its morph ends at the target.
  struct DriverDone {
    Driver driver;
  };
  // The clip jumped (output change, no motion): no morph runs any more.
  struct Jump {};
  using Event = std::variant<Begin, Snap, Retarget, Started, StartTimeout, Settle, DriverDone, Jump>;

  struct Result {
    bool accepted = false;
    Driver cancel{}; // the replaced or ended driver; zero when none
    bool wasMoving = false;
  };

  explicit IslandMorph(Face face) : m_shown(face), m_from(face) {}

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] bool settled() const noexcept { return m_phase == Phase::Settled; }
  [[nodiscard]] Face shown() const noexcept { return m_shown; }
  [[nodiscard]] Face from() const noexcept { return m_from; }
  [[nodiscard]] const Rect& fromRect() const noexcept { return m_fromRect; }
  [[nodiscard]] std::uint32_t gen() const noexcept { return m_gen; }
  [[nodiscard]] Driver driver() const noexcept { return m_driver; }

  Result apply(const Event& event) {
    const bool moving = m_phase != Phase::Settled;
    return std::visit(
        [this, moving](const auto& e) -> Result {
          using E = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<E, Begin>) {
            if (e.driver == Driver{} || e.driver == m_driver) {
              return {};
            }
            const Driver old = m_driver;
            m_from = m_shown;
            m_shown = e.to;
            m_fromRect = e.fromRect;
            m_driver = e.driver;
            m_phase = Phase::Running;
            ++m_gen;
            return {.accepted = true, .cancel = old, .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, Snap>) {
            const Driver old = end();
            m_shown = m_from = e.to;
            return {.accepted = true, .cancel = old, .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, Retarget>) {
            if (e.driver == Driver{} || (moving && e.driver != m_driver)) {
              return {};
            }
            m_driver = e.driver;
            m_phase = Phase::AwaitStart;
            ++m_gen;
            return {.accepted = true, .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, Started>) {
            if (m_phase != Phase::AwaitStart || e.gen != m_gen) {
              return {};
            }
            m_phase = Phase::Running;
            return {.accepted = true, .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, StartTimeout>) {
            if (m_phase != Phase::AwaitStart) {
              return {};
            }
            m_phase = Phase::Running;
            return {.accepted = true, .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, Settle>) {
            if (!moving) {
              return {};
            }
            return {.accepted = true, .cancel = end(), .wasMoving = moving};
          } else if constexpr (std::is_same_v<E, DriverDone>) {
            if (!moving || e.driver != m_driver) {
              return {};
            }
            end();
            return {.accepted = true, .wasMoving = moving};
          } else {
            return {.accepted = true, .cancel = end(), .wasMoving = moving};
          }
        },
        event
    );
  }

private:
  Driver end() {
    const Driver old = m_driver;
    m_driver = Driver{};
    m_from = m_shown;
    m_phase = Phase::Settled;
    return old;
  }

  Face m_shown;
  Face m_from;
  Rect m_fromRect{};
  Phase m_phase = Phase::Settled;
  std::uint32_t m_gen = 0;
  Driver m_driver{};
};
