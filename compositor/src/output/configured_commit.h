#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace andromeda {

  enum class CommitResult { Applied, Pending, Failed };
  enum class CommitOutcome { Ok, Busy, Failed };

  // The logical power state of an output; a failed configured-state commit restores the one it replaced.
  struct PowerState {
    bool desktopEnabled = false;
    bool dpmsOff = false;
    bool operator==(const PowerState&) const = default;
  };

  struct RequestedMode {
    int width = 0;
    int height = 0;
    int refreshMHz = 0;
    bool operator==(const RequestedMode&) const = default;
  };

  // `deferred`: the run waited for a frame event, so its requester already acted on Pending.
  struct ConfiguredRun {
    bool stageGeometry = false;
    PowerState restore{};
    bool deferred = false;
    bool operator==(const ConfiguredRun&) const = default;
  };

  // `restore` is set only for a synchronous failure: a deferred one keeps the requested state.
  struct ConfiguredFinish {
    CommitResult result = CommitResult::Applied;
    std::optional<PowerState> restore{};
  };

  // When an output's configured-state commit runs. A commit waits for the frame event while a page flip is pending
  // (a nonblocking commit would fail) and after EBUSY; a request_state mode waits while the frame handler commits.
  class ConfiguredCommit {
  public:
    struct Idle {
      bool operator==(const Idle&) const = default;
    };
    struct Queued {
      ConfiguredRun run{};
      bool operator==(const Queued&) const = default;
    };
    struct InFrame {
      std::optional<ConfiguredRun> queued{};
      std::optional<RequestedMode> mode{};
      bool operator==(const InFrame&) const = default;
    };
    using State = std::variant<Idle, Queued, InFrame>;

    [[nodiscard]] const State& state() const noexcept { return m_state; }
    [[nodiscard]] bool inFrame() const noexcept { return std::holds_alternative<InFrame>(m_state); }

    // Returns the run to commit now, or nothing when it waits for the frame event (the caller reports Pending).
    // Requests that wait merge: geometry is staged when any asked for it, and a failure restores the oldest state.
    [[nodiscard]] std::optional<ConfiguredRun> request(bool stageGeometry, PowerState restore, bool flipPending) {
      ConfiguredRun run{.stageGeometry = stageGeometry, .restore = restore};
      if (const std::optional<ConfiguredRun> waiting = queued()) {
        run = {.stageGeometry = stageGeometry || waiting->stageGeometry, .restore = waiting->restore};
      }
      if (flipPending) {
        setQueued(run);
        return std::nullopt;
      }
      setQueued(std::nullopt);
      return run;
    }

    // Reports the outcome of a run that request() or takeDue() returned. EBUSY is transient: it queues the run again.
    [[nodiscard]] ConfiguredFinish finish(const ConfiguredRun& run, CommitOutcome outcome) {
      switch (outcome) {
      case CommitOutcome::Ok:
        return {.result = CommitResult::Applied};
      case CommitOutcome::Failed:
        return {.result = CommitResult::Failed,
                .restore = run.deferred ? std::nullopt : std::optional{run.restore}};
      case CommitOutcome::Busy:
        break;
      }
      const std::optional<ConfiguredRun> waiting = queued();
      setQueued(ConfiguredRun{.stageGeometry = run.stageGeometry || (waiting.has_value() && waiting->stageGeometry),
                              .restore = run.restore});
      return {.result = CommitResult::Pending};
    }

    // Enters the frame handler; returns false (and changes nothing) when already in it. A queued run is then due.
    [[nodiscard]] bool beginFrame() {
      if (inFrame()) {
        return false;
      }
      std::optional<ConfiguredRun> queued{};
      if (auto* q = std::get_if<Queued>(&m_state)) {
        queued = q->run;
      }
      m_state = InFrame{.queued = queued};
      return true;
    }

    // The queued run, removed from the queue; only in the frame handler.
    [[nodiscard]] std::optional<ConfiguredRun> takeDue() {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return std::nullopt;
      }
      std::optional<ConfiguredRun> due = std::exchange(frame->queued, std::nullopt);
      if (due.has_value()) {
        due->deferred = true;
      }
      return due;
    }

    // Returns true when the mode waits for endFrame(); false when the caller commits it now.
    [[nodiscard]] bool requestMode(RequestedMode mode) {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return false;
      }
      frame->mode = mode;
      return true;
    }

    // Leaves the frame handler; returns the mode that waited for it. Outside the frame handler it changes nothing.
    [[nodiscard]] std::optional<RequestedMode> endFrame() {
      auto* frame = std::get_if<InFrame>(&m_state);
      if (frame == nullptr) {
        return std::nullopt;
      }
      const std::optional<RequestedMode> mode = frame->mode;
      if (frame->queued.has_value()) {
        m_state = Queued{.run = *frame->queued};
      } else {
        m_state = Idle{};
      }
      return mode;
    }

  private:
    [[nodiscard]] std::optional<ConfiguredRun> queued() const {
      if (const auto* frame = std::get_if<InFrame>(&m_state)) {
        return frame->queued;
      }
      if (const auto* waiting = std::get_if<Queued>(&m_state)) {
        return waiting->run;
      }
      return std::nullopt;
    }
    void setQueued(std::optional<ConfiguredRun> run) {
      if (auto* frame = std::get_if<InFrame>(&m_state)) {
        frame->queued = run;
      } else if (run.has_value()) {
        m_state = Queued{.run = *run};
      } else {
        m_state = Idle{};
      }
    }

    State m_state = Idle{};
  };

} // namespace andromeda
