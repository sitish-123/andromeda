#pragma once

#include "core/poll_source.h"
#include "shell/polkit/askpass_request.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Holds the one `andromeda-askpass` connection. Handshake: the helper sends "askpass\n"; the shell checks the caller
// and sends "verify\n"; the helper sends "ready\n" within 2 s; the shell checks again and shows the dialog. The answer is "password\n<secret>\n" or "cancel\n", or "error: <reason>\n" at any step, then the shell
// closes the connection. A peer that hangs up (its 120 s timeout) ends the request.
class AskpassBroker final : public PollSource {
public:
  using StateCallback = std::function<void()>;
  // Returns a pidfd for `pid`, or -1.
  using PidfdOpener = std::function<int(pid_t pid)>;

  explicit AskpassBroker(AskpassProc proc = {}, PidfdOpener openPidfd = {});
  ~AskpassBroker() override;

  AskpassBroker(const AskpassBroker&) = delete;
  AskpassBroker& operator=(const AskpassBroker&) = delete;

  // Called after every change of hasPendingRequest().
  void setStateCallback(StateCallback callback);

  // Takes ownership of `fd` and returns true, or writes the reason to `fd` and returns false (the caller closes
  // it). `peerPid` comes from SO_PEERCRED. A second request while one is open is refused.
  bool begin(int fd, pid_t peerPid);
  static void refuse(int fd, std::string_view reason);

  // A connection is in the handshake or shown.
  [[nodiscard]] bool busy() const noexcept { return m_pending.has_value(); }
  // The handshake passed: the dialog shows this request.
  [[nodiscard]] bool hasPendingRequest() const noexcept { return m_pending && m_pending->phase == Phase::Shown; }
  // Only valid while hasPendingRequest().
  [[nodiscard]] const AskpassRequest& pendingRequest() const { return m_pending->caller.request; }

  void submitResponse(std::string_view password);
  void cancelRequest();

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  enum class Phase { AwaitReady, Shown };

  struct Pending {
    int fd = -1;
    int peerPidfd = -1;
    int parentPidfd = -1;
    pid_t peerPid = 0;
    AskpassCaller caller;
    Phase phase = Phase::AwaitReady;
    std::string input;
    std::chrono::steady_clock::time_point readyDeadline;
  };

  void readReady();
  // Sends `reply` (none when empty), closes the connection and reports a change if the request was shown.
  void finish(std::string_view reply);

  AskpassProc m_proc;
  PidfdOpener m_openPidfd;
  std::optional<Pending> m_pending;
  StateCallback m_stateCallback;
  bool m_polled = false;
};
