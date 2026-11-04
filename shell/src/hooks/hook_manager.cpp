#include "hooks/hook_manager.h"

#include "core/log.h"

#include <cstdlib>
#include <memory>

namespace {

  constexpr Logger kLog("hooks");

} // namespace

void HookManager::setCommandRunner(CommandRunner runner) { m_runner = std::move(runner); }

void HookManager::setBlockingCommandRunner(CommandRunner runner) { m_blockingRunner = std::move(runner); }

void HookManager::setAsyncCommandRunner(AsyncCommandRunner runner) { m_asyncRunner = std::move(runner); }

void HookManager::reload(const HooksConfig& config) { m_config = config; }

void HookManager::fire(HookKind kind) const { (void)fireWithRunner(kind, m_runner); }

bool HookManager::fireBlocking(HookKind kind) const {
  return fireWithRunner(kind, m_blockingRunner ? m_blockingRunner : m_runner);
}

void HookManager::fireAsync(HookKind kind, std::function<void(bool ok)> done) const {
  if (kind == HookKind::Count || !m_asyncRunner) {
    done(false);
    return;
  }
  const auto& cmds = m_config.commands[static_cast<std::size_t>(kind)];
  if (cmds.empty()) {
    done(true);
    return;
  }
  struct Chain {
    std::vector<std::string> cmds;
    std::size_t next = 0;
    bool ok = true;
    AsyncCommandRunner runner;
    std::function<void(bool)> done;
    std::string_view name;
  };
  auto chain = std::make_shared<Chain>(Chain{cmds, 0, true, m_asyncRunner, std::move(done), hookKindKey(kind)});
  auto step = std::make_shared<std::function<void()>>();
  *step = [chain, weakStep = std::weak_ptr(step)] {
    if (chain->next >= chain->cmds.size()) {
      chain->done(chain->ok);
      return;
    }
    const std::string cmd = chain->cmds[chain->next++];
    chain->runner(cmd, [chain, cmd, next = weakStep.lock()](bool ok) {
      if (!ok) {
        kLog.warn("hook '{}' command failed: {}", chain->name, cmd);
        chain->ok = false;
      }
      (*next)();
    });
  };
  (*step)();
}

bool HookManager::fireWithRunner(HookKind kind, const CommandRunner& runner) const {
  if (kind == HookKind::Count || !runner) {
    return false;
  }
  const auto& cmds = m_config.commands[static_cast<std::size_t>(kind)];
  if (cmds.empty()) {
    return true;
  }
  const std::string_view name = hookKindKey(kind);
  kLog.debug("hook '{}' running {} command(s)", name, cmds.size());
  bool ok = true;
  for (const auto& cmd : cmds) {
    if (!runner(cmd)) {
      kLog.warn("hook '{}' command failed: {}", name, cmd);
      ok = false;
    }
  }
  return ok;
}

void HookManager::fire(HookKind kind, std::initializer_list<EnvVar> env) const {
  fireWithEnv(kind, std::span<const EnvVar>(env.begin(), env.size()));
}

void HookManager::fire(HookKind kind, const std::vector<EnvVar>& env) const { fireWithEnv(kind, env); }

void HookManager::fireWithEnv(HookKind kind, std::span<const EnvVar> env) const {
  for (const auto& [key, value] : env) {
    ::setenv(key, value.c_str(), 1);
  }
  fire(kind);
  for (const auto& [key, value] : env) {
    ::unsetenv(key);
  }
}
