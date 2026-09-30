#pragma once

#include <functional>
#include <mutex>
#include <vector>

class DeferredCall {
public:
  DeferredCall(const DeferredCall&) = delete;
  DeferredCall& operator=(const DeferredCall&) = delete;

  static void callLater(std::function<void()> fn);
  static std::vector<std::function<void()>> takePending();

private:
  static std::mutex s_mutex;
  static std::vector<std::function<void()>> s_pending;
};
