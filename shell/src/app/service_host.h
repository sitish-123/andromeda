#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct Config;
struct ConfigChangeSet;

class ServiceModule {
public:
  virtual ~ServiceModule() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  // Names of modules that must start before this one.
  [[nodiscard]] virtual std::vector<std::string_view> needs() const { return {}; }
  // True when a reload that changed these sections affects this module.
  [[nodiscard]] virtual bool reads(const ConfigChangeSet& /*change*/) const { return false; }

  virtual void start(const Config& config) = 0;
  virtual void stop() {}
  virtual void reload(const Config& /*config*/) {}
};

class ServiceHost {
public:
  ServiceHost() = default;
  ServiceHost(const ServiceHost&) = delete;
  ServiceHost& operator=(const ServiceHost&) = delete;
  ~ServiceHost();

  void add(std::unique_ptr<ServiceModule> module);
  // Adds a module and returns it for typed access; the host owns it.
  template <typename T, typename... Args> T* add(Args&&... args) {
    auto module = std::make_unique<T>(std::forward<Args>(args)...);
    T* raw = module.get();
    add(std::move(module));
    return raw;
  }

  // Starts the modules added since the last call, each after the modules it needs; earlier modules keep running.
  // Returns false and starts nothing when a need is missing or the needs form a cycle; error() then names the module.
  bool start(const Config& config);
  // Stops started modules in reverse start order.
  void stop();
  // Reloads, in start order, the started modules that read a changed section.
  void reload(const Config& config, const ConfigChangeSet& change);

  [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
  [[nodiscard]] bool computeOrder();

  std::vector<std::unique_ptr<ServiceModule>> m_modules;
  std::vector<ServiceModule*> m_order;
  std::size_t m_started = 0;
  std::string m_error;
};
