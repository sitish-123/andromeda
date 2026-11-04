#pragma once

#include "qos/policy.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Tier and freeze holds that other processes (the andromeda-system session part) place on app units through
// org.andromeda.Compositor1.Apps (docs/system-health.md §5). Pure: no D-Bus, no clock.
namespace andromeda::qos {

  class Holds {
  public:
    struct Hold {
      std::string id;
      std::string owner; // bus name; the hold ends when it leaves the bus
      std::string unit;
      std::optional<Tier> tier; // nullopt: a freeze hold
      std::string reason;
    };

    // `known`: the unit is an app scope the controller owns. Returns the hold id, or nullopt for an unknown unit.
    std::optional<std::string> holdTier(const std::string& owner, const std::string& unit, Tier tier, std::string reason,
                                        bool known);
    std::optional<std::string> holdFreeze(const std::string& owner, const std::string& unit, std::string reason,
                                          bool known);
    // Only the owner may release its hold. Returns true when a hold ended.
    bool release(const std::string& id, const std::string& owner);
    // Returns true when the owner had holds.
    bool releaseOwner(const std::string& owner);
    // A unit that ended takes its holds with it.
    void forget(const std::string& unit);

    // The policy decision with the holds on `unit`: the most throttled tier wins and a freeze hold freezes. Holds do
    // not touch an exempt app (focused, audio, call, screen cast, idle inhibitor, never_throttle).
    [[nodiscard]] Decision apply(const std::string& unit, Decision policy, bool exempt) const;

    [[nodiscard]] bool empty() const { return m_holds.empty(); }
    [[nodiscard]] std::vector<Hold> list() const;

  private:
    std::optional<std::string> add(Hold hold, bool known);

    std::map<std::string, Hold> m_holds; // by id
    std::uint64_t m_next = 1;
  };

  // One app scope as the controller last evaluated it.
  struct AppState {
    std::string dir; // cgroup directory
    bool visible = false;
    bool focused = false;
    bool exempt = false;
    Tier tier = Tier::Visible;
    bool frozen = false;
  };

  // Process-wide link between the controller and org.andromeda.Compositor1.Apps: the controller writes `apps` on each
  // evaluation and applies `holds`; the bus object places holds and calls `changed` so the controller evaluates.
  struct Registry {
    Holds holds;
    std::map<std::string, AppState> apps; // by unit
    // display.fps_cap_hidden from SetHiddenFpsCap: frame callbacks per second for hidden content that still draws
    // (background_render windows); 0 = no cap. Ends when `fpsCapOwner` leaves the bus.
    unsigned hiddenFpsCap = 0;
    std::string fpsCapOwner;
    std::function<void()> changed;
  };
  Registry& registry();

} // namespace andromeda::qos
