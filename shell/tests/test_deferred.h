#pragma once

#include "core/deferred_call.h"

#include <chrono>
#include <poll.h>

// Runs main-loop callbacks (SharedWorker results) until `done()` holds; waits on the wake fd, never past `timeout`.
template <typename Done> bool runDeferredUntil(Done done, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
    if (done()) {
      return true;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return false;
    }
    pollfd pfd{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, static_cast<int>(left.count())) > 0) {
      DeferredCall::drainWakeFd();
    }
  }
}
