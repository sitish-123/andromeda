// Runs under dbus-run-session (meson.build): a private session bus.
#include "app/single_instance.h"
#include "dbus/session_bus.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <print>
#include <thread>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "single_instance_test: {}", what);
      ++g_failures;
    }
  }

  template <typename F> bool waitFor(F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
  }
} // namespace

int main() {
  constexpr const char* kId = "dev.andromeda.SingleInstanceTest";
  expect(SingleInstance::objectPathFor("dev.andromeda.My-App") == "/dev/andromeda/My_App", "app id maps to a path");

  std::mutex lock;
  std::vector<std::string> opened;
  std::string openToken;
  std::atomic<int> activations = 0;

  SessionBus primaryBus;
  SingleInstance primary(&primaryBus, kId);
  const auto role = primary.claim(
      {}, {
              .activate = [&](const std::string&) { ++activations; },
              .open =
                  [&](const std::vector<std::string>& uris, const std::string& token) {
                    const std::scoped_lock guard(lock);
                    opened = uris;
                    openToken = token;
                  },
          }
  );
  expect(role == SingleInstance::Role::Primary, "the first claim owns the name");
  // The primary answers on its own thread, as its app loop would, while the second launch blocks in its call.
  primaryBus.connection().enterEventLoopAsync();

  ::setenv("XDG_ACTIVATION_TOKEN", "token-42", 1);
  {
    SessionBus secondBus;
    SingleInstance second(&secondBus, kId);
    expect(second.claim({"file:///tmp/a.txt", "file:///tmp/b.txt"}, {}) == SingleInstance::Role::Forwarded,
           "a second claim with files is forwarded");
  }
  expect(waitFor([&] {
           const std::scoped_lock guard(lock);
           return opened.size() == 2;
         }),
         "the primary receives Open");
  {
    const std::scoped_lock guard(lock);
    expect(opened.size() == 2 && opened[0] == "file:///tmp/a.txt", "Open carries the files in order");
    expect(openToken == "token-42", "Open carries the launch activation token");
  }
  {
    SessionBus thirdBus;
    SingleInstance third(&thirdBus, kId);
    expect(third.claim({}, {}) == SingleInstance::Role::Forwarded, "a second claim without files is forwarded");
  }
  expect(waitFor([&] { return activations.load() == 1; }), "the primary receives Activate");

  expect(SingleInstance(nullptr, kId).claim({}, {}) == SingleInstance::Role::Alone, "no bus runs alone");

  primaryBus.connection().leaveEventLoop();
  return g_failures == 0 ? 0 : 1;
}
