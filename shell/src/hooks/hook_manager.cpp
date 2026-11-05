#include "hooks/hook_manager.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <system_error>
#include <unistd.h>

namespace {

  constexpr Logger kLog("hooks");
  constexpr std::size_t kMaxFailures = 20;

  // "wallpaper_changed" -> "wallpaper-changed"
  std::string eventName(HookKind kind) {
    std::string name(hookKindKey(kind));
    std::ranges::replace(name, '_', '-');
    return name;
  }

} // namespace

void HookManager::setCommandRunner(CommandRunner runner) { m_runner = std::move(runner); }

void HookManager::setBlockingCommandRunner(CommandRunner runner) { m_blockingRunner = std::move(runner); }

void HookManager::setAsyncCommandRunner(AsyncCommandRunner runner) { m_asyncRunner = std::move(runner); }

void HookManager::setUnitRunner(UnitRunner runner, std::filesystem::path dir) {
  m_unitRunner = std::move(runner);
  m_hookDir = std::move(dir);
}

void HookManager::reload(const HooksConfig& config) { m_config = config; }

void HookManager::fire(HookKind kind) const {
  runDirHooks(kind, {}, {});
  (void)fireWithRunner(kind, m_runner);
}

bool HookManager::fireBlocking(HookKind kind) const {
  runDirHooks(kind, {}, {});
  return fireWithRunner(kind, m_blockingRunner ? m_blockingRunner : m_runner);
}

std::vector<HookManager::DirHookFailure> HookManager::dirHookFailures() const {
  const std::scoped_lock lock(m_failures->mutex);
  return m_failures->list;
}

void HookManager::runDirHooks(HookKind kind, std::span<const EnvVar> env, std::function<void()> done) const {
  std::vector<std::filesystem::path> scripts;
  const std::string event = kind == HookKind::Count ? std::string() : eventName(kind);
  if (!event.empty() && m_unitRunner && !m_hookDir.empty()) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_hookDir / (event + ".d"), ec)) {
      if (entry.is_regular_file(ec) && ::access(entry.path().c_str(), X_OK) == 0) {
        scripts.push_back(entry.path());
      }
    }
    std::ranges::sort(scripts);
  }
  if (scripts.empty()) {
    if (done) {
      done();
    }
    return;
  }
  // A transient unit gets the user manager's environment, not ours: event data goes in as --setenv.
  std::vector<std::string> envArgs;
  for (const auto& [key, value] : env) {
    envArgs.push_back(std::string("--setenv=") + key + "=" + value);
  }
  struct Chain {
    std::vector<std::filesystem::path> scripts;
    std::size_t next = 0;
    std::string event;
    std::vector<std::string> envArgs;
    UnitRunner runner;
    std::shared_ptr<Failures> failures;
    std::function<void()> done;
  };
  auto chain = std::make_shared<Chain>(
      Chain{std::move(scripts), 0, event, std::move(envArgs), m_unitRunner, m_failures, std::move(done)}
  );
  // Scripts run one after another, in name order: a later one may depend on an earlier one's result.
  auto step = std::make_shared<std::function<void()>>();
  *step = [chain, weakStep = std::weak_ptr(step)] {
    if (chain->next >= chain->scripts.size()) {
      if (chain->done) {
        chain->done();
      }
      return;
    }
    const std::string name = chain->scripts[chain->next].filename().string();
    std::vector<std::string> argv{
        "systemd-run", "--user", "--quiet", "--collect", "--wait", "--service-type=exec",
        "--property=RuntimeMaxSec=10", "--description=Andromeda hook " + chain->event + "/" + name,
    };
    argv.insert(argv.end(), chain->envArgs.begin(), chain->envArgs.end());
    argv.emplace_back("--");
    argv.push_back(chain->scripts[chain->next++].string());
    chain->runner(std::move(argv), [chain, name, next = weakStep.lock()](bool ok) {
      if (!ok) {
        kLog.warn("hook {}/{} failed", chain->event, name);
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const std::scoped_lock lock(chain->failures->mutex);
        auto& list = chain->failures->list;
        list.push_back({chain->event, name, std::chrono::duration_cast<std::chrono::seconds>(now).count()});
        if (list.size() > kMaxFailures) {
          list.erase(list.begin());
        }
      }
      (*next)();
    });
  };
  (*step)();
}

void HookManager::fireAsync(HookKind kind, std::function<void(bool ok)> done) const {
  if (kind == HookKind::Count || !m_asyncRunner) {
    done(false);
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
  auto chain = std::make_shared<Chain>(Chain{
      m_config.commands[static_cast<std::size_t>(kind)], 0, true, m_asyncRunner, std::move(done), hookKindKey(kind)
  });
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
  // done (logout, reboot, power-off) waits for the directory hooks too: the user manager stops their units.
  runDirHooks(kind, {}, [step] { (*step)(); });
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
  runDirHooks(kind, env, {});
  for (const auto& [key, value] : env) {
    ::setenv(key, value.c_str(), 1);
  }
  (void)fireWithRunner(kind, m_runner);
  for (const auto& [key, value] : env) {
    ::unsetenv(key);
  }
}
