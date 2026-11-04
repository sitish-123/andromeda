#include "dbus/andromeda/system_health_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <chrono>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

namespace {

  constexpr Logger kLog("system-health");

  const sdbus::ServiceName kName{"org.andromeda.System1"};
  const sdbus::ObjectPath kExplainPath{"/org/andromeda/System1/Explain"};
  const sdbus::ObjectPath kRulesPath{"/org/andromeda/System1/Rules"};
  const sdbus::ObjectPath kKnobsPath{"/org/andromeda/System1/Knobs"};
  constexpr auto kKnobsInterface = "org.andromeda.System1.Knobs";
  constexpr auto kExplainInterface = "org.andromeda.System1.Explain";
  constexpr auto kRulesInterface = "org.andromeda.System1.Rules";

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using Pairs = std::vector<sdbus::Struct<std::string, std::string>>;
  using RuleRow = sdbus::Struct<std::string, bool, std::string, VariantMap>;

  template <typename T> T field(const VariantMap& map, const std::string& key, T fallback) {
    const auto it = map.find(key);
    if (it == map.end()) {
      return fallback;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::vector<std::pair<std::string, std::string>> pairs(const VariantMap& map, const std::string& key) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& item : field<Pairs>(map, key, {})) {
      out.emplace_back(std::get<0>(item), std::get<1>(item));
    }
    return out;
  }

  std::uint64_t dayAgoUsec() {
    const auto now = std::chrono::system_clock::now() - std::chrono::hours(24);
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()
    );
  }

} // namespace

SystemHealthService::SystemHealthService(SessionBus& bus) : m_bus(bus) {
  m_explain = sdbus::createProxy(m_bus.connection(), kName, kExplainPath);
  m_rulesProxy = sdbus::createProxy(m_bus.connection(), kName, kRulesPath);
  m_knobs = sdbus::createProxy(m_bus.connection(), kName, kKnobsPath);
  m_explain->uponSignal("Entry").onInterface(kExplainInterface).call([this](const VariantMap& /*entry*/) {
    loadEntries();
  });
  m_knobs->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](const std::string& iface, const VariantMap& /*changed*/, const std::vector<std::string>& /*invalidated*/) {
        if (iface == kKnobsInterface) {
          loadKnobs();
        }
      });
  refresh();
  loadKnobs();
}

SystemHealthService::~SystemHealthService() = default;

void SystemHealthService::refresh() {
  loadRules();
  loadEntries();
}

void SystemHealthService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

const SystemRule* SystemHealthService::rule(const std::string& id) const {
  const auto it = std::ranges::find(m_rules, id, &SystemRule::id);
  return it == m_rules.end() ? nullptr : &*it;
}

bool SystemHealthService::running(const SystemExplainEntry& entry) const {
  if (entry.event != "started") {
    return false;
  }
  return std::ranges::none_of(m_entries, [&entry](const SystemExplainEntry& other) {
    return other.episode == entry.episode && other.event != "started";
  });
}

std::string SystemHealthService::limitedBy(const std::string& knob) const {
  for (const auto& entry : m_entries) {
    const bool holds = std::ranges::any_of(entry.holds, [&knob](const auto& hold) { return hold.first == knob; });
    if (holds && running(entry)) {
      return entry.rule;
    }
  }
  return {};
}

std::optional<double> SystemHealthService::knob(const std::string& name) const {
  const auto it = m_knobValues.find(name);
  return it == m_knobValues.end() ? std::nullopt : std::optional<double>{it->second};
}

void SystemHealthService::loadKnobs() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_knobs->getPropertyAsync("Effective").onInterface(kKnobsInterface).uponReplyInvoke(
        [this, alive](const std::optional<sdbus::Error>& error, const sdbus::Variant& value) {
          if (alive.expired() || error.has_value()) {
            return;
          }
          m_knobValues.clear();
          for (const auto& [name, v] : value.get<VariantMap>()) {
            try {
              m_knobValues[name] = v.get<double>();
            } catch (const sdbus::Error&) {
              try {
                m_knobValues[name] = v.get<bool>() ? 1.0 : 0.0;
              } catch (const sdbus::Error&) {
                // Set-valued knobs are applied by the session part itself.
              }
            }
          }
          if (m_knobCallback) {
            m_knobCallback();
          }
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("knobs: {}", e.what());
  }
}

void SystemHealthService::loadRules() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_rulesProxy->callMethodAsync("List").onInterface(kRulesInterface).uponReplyInvoke(
        [this, alive](const std::optional<sdbus::Error>& error, const std::vector<RuleRow>& rows) {
          if (alive.expired() || error.has_value()) {
            return;
          }
          m_rules.clear();
          for (const auto& [id, enabled, why, params] : rows) {
            SystemRule rule{.id = id, .enabled = enabled, .error = why};
            for (const auto& [name, value] : params) {
              try {
                rule.params[name] = value.get<double>();
              } catch (const sdbus::Error&) {
                // Switch params are not shown as rows yet.
              }
            }
            m_rules.push_back(std::move(rule));
          }
          notify();
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("rules: {}", e.what());
  }
}

void SystemHealthService::loadEntries() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_explain->callMethodAsync("List").onInterface(kExplainInterface).withArguments(dayAgoUsec()).uponReplyInvoke(
        [this, alive](const std::optional<sdbus::Error>& error, const std::vector<VariantMap>& items) {
          if (alive.expired() || error.has_value()) {
            return;
          }
          m_entries.clear();
          for (const auto& item : items) {
            m_entries.push_back(SystemExplainEntry{
                .id = field<std::string>(item, "id", ""),
                .timeUsec = field<std::uint64_t>(item, "time_usec", 0),
                .rule = field<std::string>(item, "rule", ""),
                .episode = field<std::uint64_t>(item, "episode", 0),
                .event = field<std::string>(item, "event", ""),
                .trigger = pairs(item, "trigger"),
                .holds = pairs(item, "holds"),
                .note = field<std::string>(item, "note", ""),
                .instance = field<std::string>(item, "instance", ""),
            });
          }
          std::ranges::reverse(m_entries);
          notify();
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("explain: {}", e.what());
  }
}

void SystemHealthService::setRule(const std::string& id, bool enabled, const std::map<std::string, double>& params) {
  VariantMap values;
  for (const auto& [name, value] : params) {
    values[name] = sdbus::Variant(value);
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_rulesProxy->callMethodAsync("Set")
        .onInterface(kRulesInterface)
        .withArguments(id, enabled, values)
        .uponReplyInvoke([this, alive, id](std::optional<sdbus::Error> error) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("rule {}: {}", id, error->what());
          }
          refresh();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("rule {}: {}", id, e.what());
  }
}

void SystemHealthService::undo(const std::string& entryId) {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_explain->callMethodAsync("Undo").onInterface(kExplainInterface).withArguments(entryId).uponReplyInvoke(
        [this, alive, entryId](std::optional<sdbus::Error> error) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("undo {}: {}", entryId, error->what());
          }
          loadEntries();
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.warn("undo {}: {}", entryId, e.what());
  }
}
