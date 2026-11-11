#include "auth/fingerprint_auth_machine.h"
#include "tests/test_check.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>

using fingerprint_auth::Command;
using fingerprint_auth::Commands;
using fingerprint_auth::Machine;
using fingerprint_auth::Phase;
using fingerprint_auth::Status;

namespace {

  enum class Kind : std::uint8_t { Device, Claim, VerifyStart, VerifyStatus };

  struct Message {
    Kind kind;
    std::uint64_t attempt = 0;
    bool ok = false;
    Status status = Status::InPlace;
    bool done = false;
  };

  // fprintd as the machine sees it: calls run in order, replies and signals come back late and in order, except past
  // Claim replies, which wait for polkit.
  struct World {
    Machine machine;
    std::deque<std::pair<Command, std::uint64_t>> calls;
    std::deque<Message> replies;
    bool claimed = false;
    bool verifying = false;
    bool asleep = false;

    void send(Commands commands) {
      for (const Command command :
           {fingerprint_auth::VerifyStop, fingerprint_auth::Release, fingerprint_auth::RequestDevice,
            fingerprint_auth::Claim, fingerprint_auth::VerifyStart}) {
        if ((commands & command) != 0) {
          calls.emplace_back(command, machine.attempt());
        }
      }
    }

    void serveCall() {
      const auto [command, attempt] = calls.front();
      calls.pop_front();
      switch (command) {
      case fingerprint_auth::VerifyStop:
        verifying = false;
        break;
      case fingerprint_auth::Release:
        claimed = false;
        verifying = false;
        break;
      case fingerprint_auth::RequestDevice:
        replies.push_back({Kind::Device, attempt, true});
        break;
      case fingerprint_auth::Claim:
        // fprintd refuses a second Claim from the same connection.
        TEST_CHECK(!claimed);
        replies.push_back({Kind::Claim, attempt, !claimed});
        claimed = true;
        break;
      case fingerprint_auth::VerifyStart: {
        const bool ok = claimed && !verifying;
        verifying = verifying || ok;
        replies.push_back({Kind::VerifyStart, attempt, ok});
        break;
      }
      default:
        break;
      }
    }

    [[nodiscard]] std::size_t overtakable() const {
      std::size_t count = 0;
      while (count + 1 < replies.size() && replies[count].kind == Kind::Claim) {
        ++count;
      }
      return count + 1;
    }

    void deliverReply(std::size_t index = 0) {
      const Message message = replies[index];
      replies.erase(replies.begin() + static_cast<std::ptrdiff_t>(index));
      switch (message.kind) {
      case Kind::Device:
        send(machine.deviceReply(message.attempt, message.ok));
        break;
      case Kind::Claim:
        send(machine.claimReply(message.ok));
        break;
      case Kind::VerifyStart:
        send(machine.verifyStartReply(message.attempt, message.ok, false));
        break;
      case Kind::VerifyStatus:
        send(machine.verifyStatus(message.status, message.done));
        break;
      }
    }

    void checkAlways() const {
      if (machine.phase() == Phase::Verifying) {
        TEST_CHECK(machine.claimed());
      }
      if (machine.phase() == Phase::Sleeping) {
        TEST_CHECK(asleep);
      }
      if (asleep) {
        TEST_CHECK(
            machine.phase() != Phase::RequestingDevice && machine.phase() != Phase::Claiming
            && machine.phase() != Phase::StartingVerify && machine.phase() != Phase::Verifying
        );
      }
      TEST_CHECK(machine.retries() <= fingerprint_auth::kMaxRetries);
    }

    // Once every call and reply is through, fprintd and the machine agree.
    void checkSettled() const {
      TEST_CHECK(claimed == machine.claimed());
      TEST_CHECK(verifying == (machine.phase() == Phase::Verifying));
      if (!machine.active()) {
        TEST_CHECK(!claimed);
      }
    }
  };

  void runSequence(std::uint32_t seed) {
    std::mt19937 random(seed);
    World world;
    for (int step = 0; step < 400; ++step) {
      switch (random() % 12U) {
      case 0:
        world.send(world.machine.start());
        break;
      case 1:
      case 2:
        world.send(world.machine.stop());
        break;
      case 3:
        world.send(world.machine.retryDue());
        break;
      case 4:
        world.asleep = !world.asleep;
        if (world.asleep) {
          world.verifying = false;
        }
        world.send(world.asleep ? world.machine.sleep() : world.machine.wake());
        break;
      case 5:
        if (world.verifying) {
          const auto status = static_cast<Status>(random() % 6U);
          world.replies.push_back({Kind::VerifyStatus, 0, true, status, status != Status::InPlace});
        }
        break;
      default:
        if (!world.calls.empty() && (world.replies.empty() || random() % 2U == 0U)) {
          world.serveCall();
        } else if (!world.replies.empty()) {
          world.deliverReply(random() % world.overtakable());
        }
        break;
      }
      world.checkAlways();
    }
    world.send(world.machine.stop());
    while (!world.calls.empty() || !world.replies.empty()) {
      if (!world.calls.empty()) {
        world.serveCall();
      } else {
        world.deliverReply();
      }
      world.checkAlways();
    }
    world.checkSettled();
  }

  // stop() while Claim waits for polkit: the late claim is given back, and a VerifyStart reply from the stopped
  // attempt does not show the sensor as ready.
  void lateRepliesAfterStop() {
    Machine machine;
    TEST_CHECK(machine.start() == fingerprint_auth::RequestDevice);
    TEST_CHECK(machine.deviceReply(machine.attempt(), true) == fingerprint_auth::Claim);
    TEST_CHECK(machine.stop() == fingerprint_auth::None);
    TEST_CHECK(machine.claimReply(true) == fingerprint_auth::Release);
    TEST_CHECK(!machine.claimed());

    TEST_CHECK(machine.start() == fingerprint_auth::RequestDevice);
    TEST_CHECK(machine.deviceReply(machine.attempt(), true) == fingerprint_auth::Claim);
    TEST_CHECK(machine.claimReply(true) == fingerprint_auth::VerifyStart);
    const std::uint64_t stale = machine.attempt();
    TEST_CHECK(machine.stop() == fingerprint_auth::Release);
    TEST_CHECK(machine.verifyStartReply(stale, true, false) == fingerprint_auth::None);
    TEST_CHECK(machine.phase() == Phase::Off);
  }

  // Each miss re-arms the verify until kMaxRetries re-arms ran; the next miss ends the attempt without another retry.
  void missesStopAtRetryLimit(Status miss) {
    Machine machine;
    machine.start();
    machine.deviceReply(machine.attempt(), true);
    TEST_CHECK(machine.claimReply(true) == fingerprint_auth::VerifyStart);
    machine.verifyStartReply(machine.attempt(), true, false);
    for (int retry = 1; retry <= fingerprint_auth::kMaxRetries; ++retry) {
      const Commands commands = machine.verifyStatus(miss, true);
      TEST_CHECK((commands & fingerprint_auth::VerifyStop) != 0);
      if (machine.phase() == Phase::RetryWait) {
        TEST_CHECK(commands == fingerprint_auth::VerifyStop);
        TEST_CHECK(machine.retryDue() == fingerprint_auth::VerifyStart);
      } else {
        TEST_CHECK(commands == (fingerprint_auth::VerifyStop | fingerprint_auth::VerifyStart));
      }
      TEST_CHECK(machine.phase() == Phase::StartingVerify);
      machine.verifyStartReply(machine.attempt(), true, false);
      TEST_CHECK(machine.retries() == retry);
    }
    TEST_CHECK(machine.verifyStatus(miss, true) == fingerprint_auth::VerifyStop);
    TEST_CHECK(machine.phase() == Phase::Idle);
    TEST_CHECK(machine.retryDue() == fingerprint_auth::None);
    TEST_CHECK(machine.claimed());
  }

} // namespace

int main() {
  lateRepliesAfterStop();
  missesStopAtRetryLimit(Status::NoMatch);
  missesStopAtRetryLimit(Status::Invalid);
  for (std::uint32_t seed = 1; seed <= 5000; ++seed) {
    runSequence(seed);
  }
  return 0;
}
