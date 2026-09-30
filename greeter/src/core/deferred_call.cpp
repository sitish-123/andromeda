#include "core/deferred_call.h"

#include <utility>

std::mutex DeferredCall::s_mutex;
std::vector<std::function<void()>> DeferredCall::s_pending;

void DeferredCall::callLater(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(s_mutex);
  s_pending.push_back(std::move(fn));
}

std::vector<std::function<void()>> DeferredCall::takePending() {
  std::lock_guard<std::mutex> lock(s_mutex);
  std::vector<std::function<void()>> pending = std::move(s_pending);
  return pending;
}
