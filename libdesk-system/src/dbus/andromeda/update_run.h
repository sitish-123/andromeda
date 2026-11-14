#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class UpdateRunState : std::uint8_t { Idle, Running, Done, NeedsChoice, Failed };
enum class UpdateScope : std::uint8_t { All, Andromeda };

struct UpdateRequest {
  UpdateScope scope = UpdateScope::All;
  // Not empty: UpgradeAccepting, "yes" to these questions.
  std::vector<std::string> accepted;
  bool operator==(const UpdateRequest&) const = default;
};

// The system update run as the shell sees it. Every transition returns the request to send to the helper now, if
// any. A request while a run is going waits in one slot (a later one replaces it) and starts when that run ends.
class UpdateRun {
public:
  [[nodiscard]] UpdateRunState state() const noexcept { return m_state; }
  [[nodiscard]] const std::optional<UpdateRequest>& queued() const noexcept { return m_queued; }

  [[nodiscard]] std::optional<UpdateRequest> request(UpdateRequest request) {
    if (m_state != UpdateRunState::Running) {
      return start(std::move(request));
    }
    // The run going now already does this.
    if (request.accepted.empty() && m_runningScope == request.scope) {
      return std::nullopt;
    }
    m_queued = std::move(request);
    return std::nullopt;
  }

  // A run the shell did not start (another session, the CLI, tonight's timer) goes on: its end starts ours.
  void runSeen() {
    if (m_state != UpdateRunState::Running) {
      m_state = UpdateRunState::Running;
      m_runningScope.reset();
    }
  }

  // A queued request starts at once, so Done is never seen between the two runs. A question waits for the user.
  [[nodiscard]] std::optional<UpdateRequest> finished(bool ok, bool asksQuestions) {
    if (asksQuestions) {
      m_queued.reset();
      m_state = UpdateRunState::NeedsChoice;
      return std::nullopt;
    }
    if (auto next = std::exchange(m_queued, std::nullopt)) {
      return start(std::move(*next));
    }
    m_state = ok ? UpdateRunState::Done : UpdateRunState::Failed;
    return std::nullopt;
  }

  // The helper refused our call (polkit cancel or error) or exited mid-run: nothing queued runs after it.
  void stopped(bool cancelled) {
    m_queued.reset();
    m_state = cancelled ? UpdateRunState::Idle : UpdateRunState::Failed;
  }

  // Check Now leaves a NeedsChoice or Failed result.
  void dismiss() {
    if (m_state == UpdateRunState::NeedsChoice || m_state == UpdateRunState::Failed) {
      m_state = UpdateRunState::Idle;
    }
  }

private:
  std::optional<UpdateRequest> start(UpdateRequest request) {
    m_state = UpdateRunState::Running;
    m_runningScope = request.scope;
    m_queued.reset();
    return request;
  }

  UpdateRunState m_state = UpdateRunState::Idle;
  // Empty while a run the shell did not start goes on.
  std::optional<UpdateScope> m_runningScope;
  std::optional<UpdateRequest> m_queued;
};
