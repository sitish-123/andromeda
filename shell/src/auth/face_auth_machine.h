#pragma once

#include <cstdint>

// The face unlock state. One attempt runs HasEnrolledFaces -> Claim -> VerifyStartFor. Each reply carries the attempt
// that sent it; a reply from an older attempt changes no phase; a Claim success is kept or given back.
namespace face_auth {

  enum class Phase : std::uint8_t {
    Off,
    Probing,
    Claiming,
    StartingVerify,
    Verifying,
    // A verify ended without judging a face, or a call failed: user activity starts the next attempt.
    Idle,
    RetryWait,
    // The system sleeps; the next attempt starts at wake.
    Sleeping,
    // Off for the rest of this lock: too many misses, or no gazed or no enrolled face.
    Exhausted,
    Unavailable,
  };

  // The gazed calls to send, in this bit order.
  enum Command : std::uint8_t {
    None = 0,
    VerifyStop = 1U << 0U,
    Release = 1U << 1U,
    QueryEnrolled = 1U << 2U,
    Claim = 1U << 3U,
    VerifyStart = 1U << 4U,
  };
  using Commands = std::uint8_t;

  enum class Verdict : std::uint8_t { Match, Miss, Unjudged };

  inline constexpr int kMaxMisses = 3;

  class Machine {
  public:
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool claimed() const noexcept { return m_claimed; }
    [[nodiscard]] std::uint64_t attempt() const noexcept { return m_attempt; }
    [[nodiscard]] int misses() const noexcept { return m_misses; }
    [[nodiscard]] bool active() const noexcept {
      return m_phase != Phase::Off && m_phase != Phase::Exhausted && m_phase != Phase::Unavailable;
    }

    // A new lock: clears the misses and the Exhausted or Unavailable verdict.
    Commands start() {
      if (active()) {
        return None;
      }
      m_misses = 0;
      return begin();
    }

    // After a wrong password: keeps the misses and the verdict.
    Commands resume() { return m_phase == Phase::Off ? begin() : None; }

    Commands request() {
      if (m_phase == Phase::Off || m_phase == Phase::Idle || m_phase == Phase::RetryWait) {
        return begin();
      }
      return None;
    }

    Commands activity() { return m_phase == Phase::Idle || m_phase == Phase::RetryWait ? begin() : None; }
    Commands retryDue() { return m_phase == Phase::RetryWait ? begin() : None; }

    Commands stop() {
      if (!active()) {
        return None;
      }
      return leave(Phase::Off, true);
    }

    // On request, sleep ends the request; otherwise the attempt starts again at wake.
    Commands sleep(bool onRequest) {
      m_asleep = true;
      if (!active()) {
        return None;
      }
      return leave(onRequest ? Phase::Off : Phase::Sleeping, onRequest);
    }

    Commands wake() {
      m_asleep = false;
      return m_phase == Phase::Sleeping ? begin() : None;
    }

    Commands enrolledReply(std::uint64_t attempt, bool enrolled) {
      if (!current(attempt, Phase::Probing)) {
        return None;
      }
      if (!enrolled) {
        return leave(Phase::Unavailable, true);
      }
      if (m_claimed) {
        m_phase = Phase::StartingVerify;
        return VerifyStart;
      }
      ++m_claimsInFlight;
      m_phase = Phase::Claiming;
      return Claim;
    }

    // gazed claims per connection and answers Ok to a repeated Claim, so any attempt's success is the claim of the
    // running attempt too. A reply from before gazed left the bus tells nothing.
    Commands claimReply(std::uint64_t attempt, bool ok) {
      --m_claimsInFlight;
      if (ok && attempt >= m_daemonAttempt) {
        m_claimed = true;
      }
      if (!current(attempt, Phase::Claiming)) {
        return releaseIfIdle();
      }
      if (!ok) {
        m_phase = Phase::Idle;
        return None;
      }
      m_claimed = true;
      m_phase = Phase::StartingVerify;
      return VerifyStart;
    }

    // An older verify start needs no undo: the transition that dropped it already sent VerifyStop.
    Commands verifyStartReply(std::uint64_t attempt, bool ok) {
      if (!current(attempt, Phase::StartingVerify)) {
        return None;
      }
      m_phase = ok ? Phase::Verifying : Phase::Idle;
      return None;
    }

    // A call could not be sent: the attempt waits for user activity.
    void callFailed(std::uint64_t attempt) {
      if (attempt == m_attempt && pending()) {
        if (m_phase == Phase::Claiming) {
          --m_claimsInFlight;
        }
        m_phase = Phase::Idle;
      }
    }

    Commands verifyEnded(Verdict verdict, bool onRequest) {
      if (m_phase != Phase::Verifying) {
        return None;
      }
      // gazed ended the verify itself, so leave() sends no VerifyStop.
      m_phase = Phase::Idle;
      if (verdict == Verdict::Match) {
        return leave(Phase::Off, true);
      }
      if (verdict == Verdict::Miss) {
        ++m_misses;
      }
      if (m_misses >= kMaxMisses) {
        return leave(Phase::Exhausted, true);
      }
      if (onRequest) {
        return leave(Phase::Off, true);
      }
      if (verdict == Verdict::Miss) {
        m_phase = Phase::RetryWait;
      }
      return None;
    }

    // gazed left the bus: its claim and every pending reply are gone.
    void daemonGone(bool onRequest) {
      ++m_attempt;
      m_daemonAttempt = m_attempt;
      m_claimed = false;
      if (pending() || m_phase == Phase::Verifying || (onRequest && active())) {
        m_phase = onRequest ? Phase::Off : Phase::Idle;
      }
    }

    void resetAttempts() noexcept {
      m_misses = 0;
      if (m_phase == Phase::Exhausted || m_phase == Phase::Unavailable) {
        m_phase = Phase::Off;
      }
    }

  private:
    [[nodiscard]] bool pending() const noexcept {
      return m_phase == Phase::Probing || m_phase == Phase::Claiming || m_phase == Phase::StartingVerify;
    }
    [[nodiscard]] bool current(std::uint64_t attempt, Phase expected) const noexcept {
      return attempt == m_attempt && m_phase == expected;
    }

    Commands begin() {
      ++m_attempt;
      if (m_asleep) {
        m_phase = Phase::Sleeping;
        return None;
      }
      m_phase = Phase::Probing;
      return QueryEnrolled;
    }

    // Ends the running attempt: stops a verify that runs or may start, and gives the claim back if asked.
    Commands leave(Phase next, bool release) {
      ++m_attempt;
      Commands commands = None;
      if (m_phase == Phase::Verifying || m_phase == Phase::StartingVerify) {
        commands |= VerifyStop;
      }
      m_phase = next;
      return release ? commands | releaseIfIdle() : commands;
    }

    // gazed runs calls in order, so a Release sent while a Claim is unanswered would be undone by it: wait for it.
    Commands releaseIfIdle() {
      if (active() || !m_claimed || m_claimsInFlight != 0) {
        return None;
      }
      m_claimed = false;
      return Release;
    }

    Phase m_phase = Phase::Off;
    std::uint64_t m_attempt = 0;
    int m_misses = 0;
    // gazed holds a claim for this connection, or will once the unanswered Claims run.
    bool m_claimed = false;
    std::uint32_t m_claimsInFlight = 0;
    // The first attempt after gazed last left the bus.
    std::uint64_t m_daemonAttempt = 0;
    // Mirrors logind PrepareForSleep, so a lock that starts during sleep waits for wake.
    bool m_asleep = false;
  };

} // namespace face_auth
