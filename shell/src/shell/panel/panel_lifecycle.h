#pragma once

#include <cstdint>
#include <utility>
#include <variant>

namespace panel_lifecycle {

  enum class Phase : std::uint8_t { Closed, Initializing, Open, Closing };

  struct CloseRequest {
    bool animate = true;
  };

  struct BeginInit {};
  struct InitEnded {
    bool ok = false;
  };
  struct BeginClose {};
  struct Destroyed {};

  // An open or close asked for while the surface initializes; the latest one replays when Initializing ends.
  template <typename OpenRequest> struct Requested {
    std::variant<OpenRequest, CloseRequest> request;
  };

  template <typename OpenRequest> class Lifecycle {
  public:
    using Replay = std::variant<std::monostate, OpenRequest, CloseRequest>;
    using Event = std::variant<BeginInit, InitEnded, BeginClose, Destroyed, Requested<OpenRequest>>;

    struct Result {
      bool accepted = false;
      Replay replay;
    };

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool initializing() const noexcept { return m_phase == Phase::Initializing; }
    [[nodiscard]] bool closing() const noexcept { return m_phase == Phase::Closing; }

    Result apply(Event event) {
      return std::visit([this](auto&& e) { return on(std::forward<decltype(e)>(e)); }, std::move(event));
    }

  private:
    Result on(BeginInit /*event*/) { return advance(Phase::Closed, Phase::Initializing); }

    Result on(InitEnded event) {
      Result result = advance(Phase::Initializing, event.ok ? Phase::Open : Phase::Closed);
      if (result.accepted) {
        result.replay = std::exchange(m_pending, {});
      }
      return result;
    }

    Result on(BeginClose /*event*/) { return advance(Phase::Open, Phase::Closing); }

    Result on(Destroyed /*event*/) {
      // initialize() owns the surface on the stack: refuse the destroy and replay it as a close.
      if (m_phase == Phase::Initializing) {
        m_pending = CloseRequest{.animate = false};
        return {};
      }
      m_phase = Phase::Closed;
      return {.accepted = true, .replay = {}};
    }

    Result on(Requested<OpenRequest> event) {
      if (m_phase != Phase::Initializing) {
        return {};
      }
      std::visit([this](auto&& request) { m_pending = std::forward<decltype(request)>(request); }, std::move(event.request));
      return {.accepted = true, .replay = {}};
    }

    Result advance(Phase from, Phase to) {
      if (m_phase != from) {
        return {};
      }
      m_phase = to;
      return {.accepted = true, .replay = {}};
    }

    Phase m_phase = Phase::Closed;
    Replay m_pending;
  };

} // namespace panel_lifecycle
