#include "auth/face_auth_machine.h"
#include "tests/test_check.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>

using face_auth::Command;
using face_auth::Commands;
using face_auth::Machine;
using face_auth::Phase;
using face_auth::Verdict;

namespace {

  enum class Kind : std::uint8_t { Enrolled, Claim, VerifyStart, VerifyEnded };

  struct Message {
    Kind kind;
    std::uint64_t attempt = 0;
    bool ok = false;
    Verdict verdict = Verdict::Unjudged;
  };

  // gazed as the machine sees it: calls run in order, replies and signals come back late and in order, except past Claim replies.
  struct World {
    Machine machine;
    std::deque<std::pair<Command, std::uint64_t>> calls;
    std::deque<Message> replies;
    bool claimed = false;
    bool verifying = false;
    bool asleep = false;
    bool onRequest = false;

    void send(Commands commands) {
      for (const Command command :
           {face_auth::VerifyStop, face_auth::Release, face_auth::QueryEnrolled, face_auth::Claim,
            face_auth::VerifyStart}) {
        if ((commands & command) != 0) {
          calls.emplace_back(command, machine.attempt());
        }
      }
    }

    void serveCall() {
      const auto [command, attempt] = calls.front();
      calls.pop_front();
      switch (command) {
      case face_auth::VerifyStop:
        verifying = false;
        break;
      case face_auth::Release:
        claimed = false;
        verifying = false;
        break;
      case face_auth::QueryEnrolled:
        replies.push_back({Kind::Enrolled, attempt, true});
        break;
      case face_auth::Claim:
        replies.push_back({Kind::Claim, attempt, true});
        claimed = true;
        break;
      case face_auth::VerifyStart: {
        const bool ok = claimed && !verifying;
        verifying = verifying || ok;
        replies.push_back({Kind::VerifyStart, attempt, ok});
        break;
      }
      default:
        break;
      }
    }

    // gazed runs method calls concurrently and Claim waits for polkit, so any reply can overtake pending Claim replies.
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
      case Kind::Enrolled:
        send(machine.enrolledReply(message.attempt, message.ok));
        break;
      case Kind::Claim:
        send(machine.claimReply(message.attempt, message.ok));
        break;
      case Kind::VerifyStart:
        send(machine.verifyStartReply(message.attempt, message.ok));
        break;
      case Kind::VerifyEnded:
        send(machine.verifyEnded(message.verdict, onRequest));
        break;
      }
    }

    void daemonGone() {
      for (const auto& [command, attempt] : calls) {
        if (command == face_auth::QueryEnrolled) {
          replies.push_back({Kind::Enrolled, attempt, false});
        } else if (command == face_auth::Claim) {
          replies.push_back({Kind::Claim, attempt, false});
        } else if (command == face_auth::VerifyStart) {
          replies.push_back({Kind::VerifyStart, attempt, false});
        }
      }
      calls.clear();
      claimed = false;
      verifying = false;
      machine.daemonGone(onRequest);
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
            machine.phase() != Phase::Probing
            && machine.phase() != Phase::Claiming
            && machine.phase() != Phase::StartingVerify
            && machine.phase() != Phase::Verifying
        );
      }
      TEST_CHECK(machine.misses() <= face_auth::kMaxMisses);
    }

    // Once every call and reply is through, gazed and the machine agree.
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
    world.onRequest = (seed % 3U) == 0U;
    for (int step = 0; step < 400; ++step) {
      switch (random() % 14U) {
      case 0:
        world.send(world.machine.start());
        break;
      case 1:
        world.send(world.machine.resume());
        break;
      case 2:
        world.send(world.machine.stop());
        break;
      case 3:
        world.send(world.machine.request());
        break;
      case 4:
        world.send(world.machine.activity());
        break;
      case 5:
        world.send(world.machine.retryDue());
        break;
      case 6:
        world.asleep = !world.asleep;
        world.send(world.asleep ? world.machine.sleep(world.onRequest) : world.machine.wake());
        break;
      case 7:
        if (random() % 8U == 0U) {
          world.daemonGone();
        }
        break;
      case 8:
        if (world.verifying) {
          world.verifying = false;
          world.replies.push_back({Kind::VerifyEnded, 0, true, static_cast<Verdict>(random() % 3U)});
        }
        break;
      case 9:
        if (random() % 16U == 0U) {
          world.machine.resetAttempts();
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

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 5000; ++seed) {
    runSequence(seed);
  }
  return 0;
}
