#pragma once

#include <cstdint>

// The fingerprint unlock state. One attempt runs GetDefaultDevice -> Claim -> VerifyStart. Device and VerifyStart
// replies carry the attempt that sent them; a reply from an older attempt changes no phase. fprintd claims per
// connection, so a Claim reply is kept for any attempt, or given back when no attempt runs.
namespace fingerprint_auth {

  enum class Phase : std::uint8_t {
    Off,
    RequestingDevice,
    Claiming,
    StartingVerify,
    Verifying,
    RetryWait,
    // VerifyStart lost the claim (suspend can drop it): the device is requested and claimed again once.
    ReclaimWait,
    // The system sleeps; the next attempt starts at wake.
    Sleeping,
    // A match, too many misses or a failed call: the next attempt starts at wake.
    Idle,
    // verify-disconnected: no attempt until the next start().
    Aborted,
  };

  // The fprintd calls to send, in this bit order.
  enum Command : std::uint8_t {
    None = 0,
    VerifyStop = 1U << 0U,
    Release = 1U << 1U,
    RequestDevice = 1U << 2U,
    Claim = 1U << 3U,
    VerifyStart = 1U << 4U,
  };
  using Commands = std::uint8_t;

  enum class Status : std::uint8_t { Match, NoMatch, Error, InPlace, Disconnected, Invalid };

  inline constexpr int kMaxRetries = 3;

  class Machine {
  public:
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool claimed() const noexcept { return m_claimed; }
    [[nodiscard]] std::uint64_t attempt() const noexcept { return m_attempt; }
    [[nodiscard]] int retries() const noexcept { return m_retries; }
    [[nodiscard]] bool active() const noexcept { return m_phase != Phase::Off; }
    [[nodiscard]] bool current(std::uint64_t attempt, Phase expected) const noexcept {
      return attempt == m_attempt && m_phase == expected;
    }

    Commands start() {
      if (active()) {
        return None;
      }
      m_retries = 0;
      m_reclaimed = false;
      return begin(false);
    }

    Commands stop() {
      if (!active()) {
        return None;
      }
      ++m_attempt;
      m_phase = Phase::Off;
      return releaseIfIdle();
    }

    // fprintd handles sleep itself and a call sent now may never be answered, so sleep sends nothing.
    Commands sleep() {
      m_asleep = true;
      if (m_phase != Phase::Off && m_phase != Phase::Aborted) {
        ++m_attempt;
        m_phase = Phase::Sleeping;
      }
      return None;
    }

    // A verify that was starting at sleep may run now: stop it before the next VerifyStart.
    Commands wake() {
      m_asleep = false;
      if (m_phase != Phase::Sleeping) {
        return None;
      }
      return (m_claimed ? VerifyStop : None) | begin(false);
    }

    Commands deviceReply(std::uint64_t attempt, bool ok) {
      if (!current(attempt, Phase::RequestingDevice)) {
        return None;
      }
      if (!ok) {
        m_phase = Phase::Idle;
        return None;
      }
      m_deviceKnown = true;
      return claim();
    }

    Commands claimReply(bool ok) {
      m_claimInFlight = false;
      m_claimed = m_claimed || ok;
      if (m_phase != Phase::Claiming) {
        return releaseIfIdle();
      }
      if (!m_claimed) {
        m_phase = Phase::Idle;
        return None;
      }
      m_phase = Phase::StartingVerify;
      return VerifyStart;
    }

    Commands verifyStartReply(std::uint64_t attempt, bool ok, bool claimLost) {
      if (!current(attempt, Phase::StartingVerify)) {
        return None;
      }
      if (ok) {
        m_phase = Phase::Verifying;
        m_reclaimed = false;
        m_retries += m_countRetry ? 1 : 0;
      } else if (claimLost && !m_reclaimed) {
        m_reclaimed = true;
        m_phase = Phase::ReclaimWait;
      } else {
        m_phase = Phase::Idle;
      }
      return None;
    }

    // A call could not be sent: the attempt waits for wake or the next start().
    void callFailed(std::uint64_t attempt) {
      if (attempt != m_attempt) {
        return;
      }
      if (m_phase == Phase::Claiming) {
        m_claimInFlight = false;
      }
      if (m_phase == Phase::RequestingDevice || m_phase == Phase::Claiming || m_phase == Phase::StartingVerify) {
        m_phase = Phase::Idle;
      }
    }

    Commands retryDue() {
      if (m_phase == Phase::RetryWait) {
        return begin(true);
      }
      if (m_phase == Phase::ReclaimWait) {
        m_claimed = false;
        m_deviceKnown = false;
        return begin(false);
      }
      return None;
    }

    Commands verifyStatus(Status status, bool done) {
      if (m_phase == Phase::Sleeping) {
        return VerifyStop;
      }
      if (m_phase != Phase::Verifying) {
        return None;
      }
      switch (status) {
      case Status::Match:
        ++m_attempt;
        m_phase = Phase::Idle;
        return VerifyStop;
      case Status::NoMatch:
      case Status::Error:
        ++m_attempt;
        m_phase = m_retries >= kMaxRetries ? Phase::Idle : Phase::RetryWait;
        return VerifyStop;
      case Status::Disconnected:
        ++m_attempt;
        m_phase = Phase::Aborted;
        return VerifyStop;
      case Status::Invalid:
        if (!done) {
          return None;
        }
        if (m_retries >= kMaxRetries) {
          ++m_attempt;
          m_phase = Phase::Idle;
          return VerifyStop;
        }
        return VerifyStop | begin(true);
      case Status::InPlace:
        break;
      }
      return None;
    }

  private:
    Commands begin(bool isRetry) {
      ++m_attempt;
      m_countRetry = isRetry;
      if (m_asleep) {
        m_phase = Phase::Sleeping;
        return None;
      }
      if (!m_deviceKnown) {
        m_phase = Phase::RequestingDevice;
        return RequestDevice;
      }
      if (m_claimInFlight) {
        m_phase = Phase::Claiming;
        return None;
      }
      if (!m_claimed) {
        return claim();
      }
      m_phase = Phase::StartingVerify;
      return VerifyStart;
    }

    Commands claim() {
      m_claimInFlight = true;
      m_phase = Phase::Claiming;
      return Claim;
    }

    // fprintd may run a Release sent while a Claim waits for polkit before that Claim: wait for its reply.
    Commands releaseIfIdle() {
      if (active() || m_claimInFlight) {
        return None;
      }
      m_deviceKnown = false;
      if (!m_claimed) {
        return None;
      }
      m_claimed = false;
      return Release;
    }

    Phase m_phase = Phase::Off;
    std::uint64_t m_attempt = 0;
    int m_retries = 0;
    bool m_claimed = false;
    bool m_claimInFlight = false;
    // A device proxy exists for this attempt; stop() drops it so the next start asks fprintd again.
    bool m_deviceKnown = false;
    bool m_reclaimed = false;
    // The running VerifyStart re-arms after a miss and counts toward kMaxRetries.
    bool m_countRetry = false;
    // Mirrors logind PrepareForSleep, so an attempt that starts during sleep waits for wake.
    bool m_asleep = false;
  };

} // namespace fingerprint_auth
