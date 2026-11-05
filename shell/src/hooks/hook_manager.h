#pragma once

#include "config/config_types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

class HookManager {
public:
  using CommandRunner = std::function<bool(const std::string& command)>;
  using AsyncCommandRunner = std::function<void(const std::string& command, std::function<void(bool ok)> done)>;
  using EnvVar = std::pair<const char*, std::string>;
  // Starts argv as a transient user unit; done(ok) when it exits.
  using UnitRunner = std::function<void(std::vector<std::string> argv, std::function<void(bool ok)> done)>;

  struct DirHookFailure {
    std::string event;
    std::string script;
    std::int64_t unixTime = 0;
  };

  void setCommandRunner(CommandRunner runner);
  void setBlockingCommandRunner(CommandRunner runner);
  void setAsyncCommandRunner(AsyncCommandRunner runner);
  // Executables in <dir>/<event>.d/ run on each event (default dir: $XDG_CONFIG_HOME/andromeda/hooks).
  void setUnitRunner(UnitRunner runner, std::filesystem::path dir);
  void reload(const HooksConfig& config);
  void fire(HookKind kind) const;
  [[nodiscard]] bool fireBlocking(HookKind kind) const;
  // Runs the hook's commands one after another; done(ok) runs after the last one finishes.
  void fireAsync(HookKind kind, std::function<void(bool ok)> done) const;
  void fire(HookKind kind, std::initializer_list<EnvVar> env) const;
  void fire(HookKind kind, const std::vector<EnvVar>& env) const;

  [[nodiscard]] const HooksConfig& config() const noexcept { return m_config; }
  // Newest last, at most 20. Units may exit on another thread; this returns a copy.
  [[nodiscard]] std::vector<DirHookFailure> dirHookFailures() const;

private:
  [[nodiscard]] bool fireWithRunner(HookKind kind, const CommandRunner& runner) const;
  void fireWithEnv(HookKind kind, std::span<const EnvVar> env) const;
  // Runs <dir>/<event>.d/* in name order, one after another; done (may be empty) after the last one.
  void runDirHooks(HookKind kind, std::span<const EnvVar> env, std::function<void()> done) const;

  struct Failures {
    std::mutex mutex;
    std::vector<DirHookFailure> list;
  };

  HooksConfig m_config;
  CommandRunner m_runner;
  CommandRunner m_blockingRunner;
  AsyncCommandRunner m_asyncRunner;
  UnitRunner m_unitRunner;
  std::filesystem::path m_hookDir;
  std::shared_ptr<Failures> m_failures = std::make_shared<Failures>();
};
