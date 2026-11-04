#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/TypeTraits.h>
#include <string>
#include <vector>

namespace sdbus {
  class IConnection;
  class IProxy;
} // namespace sdbus

// A running .service or .scope unit. Metadata only: CPU and memory come from the unit's cgroup files.
struct SystemdUnit {
  std::string name;
  std::string objectPath;
  std::string description;
  std::string activeState;
  std::string subState;
  std::string appId; // appIdFromUnit(name); "" for plain services
  std::string slice;
  std::string controlGroup; // relative to the cgroup2 mount, e.g. /user.slice/.../opencode.service
  std::string fragmentPath;
  std::string unitFileState;  // enabled, disabled, static, generated, transient, ...
  std::string unitFilePreset; // vendor preset: enabled or disabled
  std::uint64_t activeEnterUsec = 0; // CLOCK_REALTIME
  bool systemManager = false;
};

// Client of one systemd manager (user manager on the session bus, system manager on the system bus).
// Lists running units on refresh(); with setLive(true) it also follows UnitNew / UnitRemoved / JobRemoved.
class SystemdUnitsService {
public:
  enum class Manager : std::uint8_t { User, System };
  using ChangeCallback = std::function<void()>;

  SystemdUnitsService(sdbus::IConnection& connection, Manager manager, ChangeCallback onChange = {});
  ~SystemdUnitsService();

  SystemdUnitsService(const SystemdUnitsService&) = delete;
  SystemdUnitsService& operator=(const SystemdUnitsService&) = delete;

  // False until the first ListUnits reply, and after a failed one.
  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }
  // Sorted by name. Complete only after onChange: details arrive asynchronously.
  [[nodiscard]] const std::vector<SystemdUnit>& units() const noexcept { return m_units; }

  // onChange fires once when the list and every unit's details have arrived.
  void refresh();
  // Each signal wakes the shell, so live updates stay off while no one shows the list.
  void setLive(bool live);

private:
  void loadDetails(const std::string& name, const std::string& path, std::uint64_t generation);
  void loadUnit(const std::string& name, const std::string& path);
  void removeUnit(const std::string& name);
  void finishOne(std::uint64_t generation);
  sdbus::IProxy& unitProxy(const std::string& path);
  void notify();

  sdbus::IConnection& m_connection;
  Manager m_manager;
  ChangeCallback m_onChange;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> m_unitProxies; // by object path
  std::vector<SystemdUnit> m_units;
  std::vector<sdbus::Slot> m_signalSlots;
  std::uint64_t m_generation = 0;
  int m_pending = 0;
  bool m_available = false;
  std::string m_lastError;
};
