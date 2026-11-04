#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// One rule of the session part of andromeda-system (docs/system-health.md §4.3), as a Settings row.
struct SystemRule {
  std::string id;
  bool enabled = false;
  // Why the rule cannot run; empty when it loaded.
  std::string error;
  std::map<std::string, double> params;
};

// One explain log entry (docs/system-health.md §6).
struct SystemExplainEntry {
  std::string id;
  std::uint64_t timeUsec = 0;
  std::string rule;
  std::uint64_t episode = 0;
  // started, cleared, undone.
  std::string event;
  std::vector<std::pair<std::string, std::string>> trigger;
  std::vector<std::pair<std::string, std::string>> holds;
  std::string note;
};

// Client of org.andromeda.System1 on the session bus: rules (Settings rows) and the explain log of the last 24 h.
class SystemHealthService {
public:
  explicit SystemHealthService(SessionBus& bus);
  ~SystemHealthService();

  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }
  void refresh();

  [[nodiscard]] const std::vector<SystemRule>& rules() const noexcept { return m_rules; }
  [[nodiscard]] const SystemRule* rule(const std::string& id) const;
  // Newest first.
  [[nodiscard]] const std::vector<SystemExplainEntry>& entries() const noexcept { return m_entries; }
  // A started entry whose episode has not ended: its holds are in force and Undo applies.
  [[nodiscard]] bool running(const SystemExplainEntry& entry) const;
  // The running rule that holds `knob`, or empty.
  [[nodiscard]] std::string limitedBy(const std::string& knob) const;

  // A session knob's effective value while a hold sets it (display.brightness_step: -1 = one step dimmer).
  [[nodiscard]] std::optional<double> knob(const std::string& name) const;
  void setKnobCallback(std::function<void()> callback) { m_knobCallback = std::move(callback); }

  void setRule(const std::string& id, bool enabled, const std::map<std::string, double>& params);
  void undo(const std::string& entryId);

private:
  void loadRules();
  void loadEntries();
  void loadKnobs();
  void notify();

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_explain;
  std::unique_ptr<sdbus::IProxy> m_rulesProxy;
  std::unique_ptr<sdbus::IProxy> m_knobs;
  std::map<std::string, double> m_knobValues;
  std::function<void()> m_knobCallback;
  std::vector<SystemRule> m_rules;
  std::vector<SystemExplainEntry> m_entries;
  std::function<void()> m_changeCallback;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
};
