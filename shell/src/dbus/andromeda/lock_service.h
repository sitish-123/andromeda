#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SessionBus;

// org.andromeda.Lock1: the `noctalia lock` process's control surface for the shell. The process also answers logind
// Lock/Unlock and PrepareForSleep on its own, so the session locks with the shell down.
class LockDBusService {
public:
  struct Handlers {
    // Reasons: "user", "idle", "sleep", "phone". False when the lock screen is off in the config.
    std::function<bool(const std::string& reason)> lock;
    std::function<void()> primeCapture;
    std::function<void()> clearCapture;
    // The lock fades out before the outputs go dark; the callback gets false when it could not.
    std::function<void(std::function<void(bool)>)> prepareOutputPowerOff;
    std::function<void()> cancelOutputPowerOff;
    std::function<void()> outputsWoke;
    // A suspend the shell starts itself (plain Suspend, or lock-then-suspend) skips the lock before sleep.
    std::function<void(bool)> skipLockOnNextSleep;
    std::function<bool()> active;
    std::function<bool()> locked;
  };

  LockDBusService(SessionBus& bus, Handlers handlers);

  // state: "locked", "unlocked" or "aborted". Also signals Active and Locked through PropertiesChanged.
  void stateChanged(const std::string& state);
  // Times (Unix ms) of the passwords rejected while locked, sent once at unlock.
  void failedAttempts(const std::vector<std::int64_t>& rejectedAtMs);

private:
  Handlers m_handlers;
  std::unique_ptr<sdbus::IObject> m_object;
};
