#include "dbus/systemd/systemd_units_service.h"

#include "core/log.h"
#include "dbus/systemd/systemd_unit_name.h"

#include <algorithm>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <utility>

namespace {

  constexpr Logger kLog("systemd-units");
  const sdbus::ServiceName kBusName{"org.freedesktop.systemd1"};
  const sdbus::ObjectPath kManagerPath{"/org/freedesktop/systemd1"};
  constexpr auto kManagerInterface = "org.freedesktop.systemd1.Manager";
  constexpr auto kUnitInterface = "org.freedesktop.systemd1.Unit";

  // ListUnits: name, description, load, active, sub, following, path, job id, job type, job path.
  using UnitRow = sdbus::Struct<
      std::string, std::string, std::string, std::string, std::string, std::string, sdbus::ObjectPath, std::uint32_t,
      std::string, sdbus::ObjectPath>;
  using Properties = std::map<sdbus::PropertyName, sdbus::Variant>;

  bool tracked(std::string_view name) { return name.ends_with(".service") || name.ends_with(".scope"); }

  bool running(std::string_view activeState) {
    return activeState == "active" || activeState == "activating" || activeState == "reloading"
           || activeState == "deactivating";
  }

  template <typename T> T valueOr(const Properties& props, const char* key, T fallback) {
    const auto it = props.find(sdbus::PropertyName{key});
    if (it == props.end()) {
      return fallback;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  void applyUnitProperties(SystemdUnit& unit, const Properties& props) {
    unit.description = valueOr<std::string>(props, "Description", unit.description);
    unit.activeState = valueOr<std::string>(props, "ActiveState", unit.activeState);
    unit.subState = valueOr<std::string>(props, "SubState", unit.subState);
    unit.fragmentPath = valueOr<std::string>(props, "FragmentPath", {});
    unit.unitFileState = valueOr<std::string>(props, "UnitFileState", {});
    unit.unitFilePreset = valueOr<std::string>(props, "UnitFilePreset", {});
    unit.activeEnterUsec = valueOr<std::uint64_t>(props, "ActiveEnterTimestamp", 0);
  }

  const char* typeInterface(std::string_view name) {
    return name.ends_with(".scope") ? "org.freedesktop.systemd1.Scope" : "org.freedesktop.systemd1.Service";
  }

} // namespace

SystemdUnitsService::SystemdUnitsService(sdbus::IConnection& connection, Manager manager, ChangeCallback onChange)
    : m_connection(connection), m_manager(manager), m_onChange(std::move(onChange)) {
  try {
    m_proxy = sdbus::createProxy(m_connection, kBusName, kManagerPath);
  } catch (const sdbus::Error& e) {
    m_lastError = e.what();
    kLog.warn("systemd1 unavailable: {}", e.what());
  }
}

SystemdUnitsService::~SystemdUnitsService() = default;

void SystemdUnitsService::notify() {
  if (m_onChange) {
    m_onChange();
  }
}

sdbus::IProxy& SystemdUnitsService::unitProxy(const std::string& path) {
  auto& proxy = m_unitProxies[path];
  if (proxy == nullptr) {
    proxy = sdbus::createProxy(m_connection, kBusName, sdbus::ObjectPath{path});
  }
  return *proxy;
}

void SystemdUnitsService::refresh() {
  if (m_proxy == nullptr) {
    notify();
    return;
  }
  const std::uint64_t generation = ++m_generation;
  m_proxy->callMethodAsync("ListUnits")
      .onInterface(kManagerInterface)
      .uponReplyInvoke([this, generation](const std::optional<sdbus::Error>& error, const std::vector<UnitRow>& rows) {
        if (generation != m_generation) {
          return;
        }
        if (error.has_value()) {
          m_available = false;
          m_lastError = error->what();
          kLog.warn("ListUnits failed: {}", m_lastError);
          notify();
          return;
        }
        m_available = true;
        m_lastError.clear();
        std::vector<SystemdUnit> units;
        for (const auto& row : rows) {
          const std::string& name = row.get<0>();
          if (!tracked(name) || !running(row.get<3>())) {
            continue;
          }
          SystemdUnit unit;
          unit.name = name;
          unit.description = row.get<1>();
          unit.activeState = row.get<3>();
          unit.subState = row.get<4>();
          unit.objectPath = row.get<6>();
          unit.appId = appIdFromUnit(name);
          unit.systemManager = m_manager == Manager::System;
          units.push_back(std::move(unit));
        }
        std::ranges::sort(units, {}, &SystemdUnit::name);
        std::erase_if(m_unitProxies, [&units](const auto& entry) {
          return std::ranges::none_of(units, [&entry](const SystemdUnit& u) { return u.objectPath == entry.first; });
        });
        m_units = std::move(units);
        m_pending = static_cast<int>(m_units.size()) * 2;
        if (m_pending == 0) {
          notify();
          return;
        }
        for (const auto& unit : m_units) {
          loadDetails(unit.name, unit.objectPath, generation);
        }
      });
}

void SystemdUnitsService::finishOne(std::uint64_t generation) {
  if (generation == m_generation && --m_pending == 0) {
    notify();
  }
}

void SystemdUnitsService::loadDetails(const std::string& name, const std::string& path, std::uint64_t generation) {
  auto& proxy = unitProxy(path);
  proxy.getAllPropertiesAsync().onInterface(kUnitInterface).uponReplyInvoke(
      [this, name, generation](const std::optional<sdbus::Error>& error, const Properties& props) {
        if (generation != m_generation) {
          return;
        }
        auto it = std::ranges::find(m_units, name, &SystemdUnit::name);
        if (!error.has_value() && it != m_units.end()) {
          applyUnitProperties(*it, props);
        }
        finishOne(generation);
      }
  );
  proxy.getAllPropertiesAsync().onInterface(typeInterface(name)).uponReplyInvoke(
      [this, name, generation](const std::optional<sdbus::Error>& error, const Properties& props) {
        if (generation != m_generation) {
          return;
        }
        auto it = std::ranges::find(m_units, name, &SystemdUnit::name);
        if (!error.has_value() && it != m_units.end()) {
          it->slice = valueOr<std::string>(props, "Slice", {});
          it->controlGroup = valueOr<std::string>(props, "ControlGroup", {});
        }
        finishOne(generation);
      }
  );
}

void SystemdUnitsService::removeUnit(const std::string& name) {
  const auto it = std::ranges::find(m_units, name, &SystemdUnit::name);
  if (it == m_units.end()) {
    return;
  }
  // The unit proxy stays until the next refresh(): this may run inside that proxy's own reply.
  m_units.erase(it);
  notify();
}

// One unit changed outside refresh(): read its state, then add, update or drop it.
void SystemdUnitsService::loadUnit(const std::string& name, const std::string& path) {
  if (!tracked(name)) {
    return;
  }
  unitProxy(path).getAllPropertiesAsync().onInterface(kUnitInterface).uponReplyInvoke(
      [this, name, path](const std::optional<sdbus::Error>& error, const Properties& props) {
        if (error.has_value() || !running(valueOr<std::string>(props, "ActiveState", {}))) {
          removeUnit(name);
          return;
        }
        auto it = std::ranges::lower_bound(m_units, name, {}, &SystemdUnit::name);
        if (it == m_units.end() || it->name != name) {
          SystemdUnit unit;
          unit.name = name;
          unit.objectPath = path;
          unit.appId = appIdFromUnit(name);
          unit.systemManager = m_manager == Manager::System;
          it = m_units.insert(it, std::move(unit));
        }
        applyUnitProperties(*it, props);
        unitProxy(path).getAllPropertiesAsync().onInterface(typeInterface(name)).uponReplyInvoke(
            [this, name](const std::optional<sdbus::Error>& typeError, const Properties& typeProps) {
              auto unit = std::ranges::find(m_units, name, &SystemdUnit::name);
              if (unit == m_units.end()) {
                return;
              }
              if (!typeError.has_value()) {
                unit->slice = valueOr<std::string>(typeProps, "Slice", {});
                unit->controlGroup = valueOr<std::string>(typeProps, "ControlGroup", {});
              }
              notify();
            }
        );
      }
  );
}

void SystemdUnitsService::setLive(bool live) {
  if (m_proxy == nullptr || live == !m_signalSlots.empty()) {
    return;
  }
  if (!live) {
    m_signalSlots.clear();
    m_proxy->callMethodAsync("Unsubscribe").onInterface(kManagerInterface).uponReplyInvoke([](const std::optional<sdbus::Error>&) {});
    return;
  }
  m_signalSlots.push_back(m_proxy->uponSignal("UnitNew").onInterface(kManagerInterface).call(
      [this](const std::string& name, const sdbus::ObjectPath& path) { loadUnit(name, path); }, sdbus::return_slot
  ));
  m_signalSlots.push_back(m_proxy->uponSignal("UnitRemoved").onInterface(kManagerInterface).call(
      [this](const std::string& name, const sdbus::ObjectPath& /*path*/) { removeUnit(name); }, sdbus::return_slot
  ));
  m_signalSlots.push_back(m_proxy->uponSignal("JobRemoved").onInterface(kManagerInterface).call(
      [this](std::uint32_t /*id*/, const sdbus::ObjectPath& /*job*/, const std::string& name, const std::string& /*result*/) {
        if (!tracked(name)) {
          return;
        }
        // JobRemoved carries no unit path; ask the manager for it.
        m_proxy->callMethodAsync("GetUnit").onInterface(kManagerInterface).withArguments(name).uponReplyInvoke(
            [this, name](const std::optional<sdbus::Error>& error, const sdbus::ObjectPath& path) {
              if (error.has_value()) {
                removeUnit(name);
              } else {
                loadUnit(name, path);
              }
            }
        );
      },
      sdbus::return_slot
  ));
  m_proxy->callMethodAsync("Subscribe").onInterface(kManagerInterface).uponReplyInvoke([](const std::optional<sdbus::Error>& error) {
    if (error.has_value()) {
      kLog.warn("Subscribe failed: {}", error->what());
    }
  });
}
