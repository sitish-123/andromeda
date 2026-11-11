#pragma once

#include "config/keybind_parse.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace andromeda {

  struct ResolvedWindowRule;

  // Where a window was: its output, workspace (a position from 1 or a name), float or tile, fullscreen, and floating
  // size.
  struct SessionPlacement {
    std::string output;
    std::optional<WorkspaceReference> workspace;
    bool floating = false;
    bool fullscreen = false;
    int width = 0;
    int height = 0;
    bool operator==(const SessionPlacement&) const = default;
  };

  // Makes the next window that `rule` opens follow `placement`.
  void applySessionPlacement(const SessionPlacement& placement, ResolvedWindowRule& rule);

  // A workspace reference in JSON: a number is a position from 1, a string is a name.
  nlohmann::json workspaceReferenceJson(const WorkspaceReference& reference);
  std::optional<WorkspaceReference> workspaceReferenceFrom(const nlohmann::json& value);

  nlohmann::json sessionPlacementJson(const SessionPlacement& placement);
  // Nullopt unless `value` is an object; a missing or wrongly typed field keeps its default.
  std::optional<SessionPlacement> sessionPlacementFrom(const nlohmann::json& value);

  // One toplevel of a live xdg_session_v1, by name. Free: the name has no live toplevel. Restoring: restore_toplevel
  // found a stored placement and `restored` is not sent yet. Configured: `restored` went out with the first configure;
  // the placement holds until the window maps. Tracking: the compositor records the window's placement.
  enum class SessionEntryState : uint8_t { Free, Restoring, Configured, Tracking };

  // Add: add_toplevel. Restore: restore_toplevel of a stored name; RestoreUnknown: of a name the store lacks.
  // ResourceDestroyed: the client destroyed the xdg_toplevel_session_v1, which leaves window management as it is.
  // Remove: remove_toplevel, or the session went away.
  enum class SessionEntryEvent : uint8_t {
    Add,
    Restore,
    RestoreUnknown,
    Configure,
    Map,
    ResourceDestroyed,
    ToplevelDestroyed,
    Remove,
  };

  // The next state, or nullopt when `event` is invalid in `from` (an Add or Restore of a name in use, a Configure or
  // Map with no placement to apply).
  std::optional<SessionEntryState> transitionSessionEntry(SessionEntryState from, SessionEntryEvent event);

  // True when the step from `from` to `to` sends xdg_toplevel_session_v1.restored.
  constexpr bool sessionEntrySendsRestored(SessionEntryState from, SessionEntryState to) {
    return from == SessionEntryState::Restoring
        && (to == SessionEntryState::Configured || to == SessionEntryState::Tracking);
  }

  // Session id -> toplevel name -> placement. Holds at most kMaxSessions sessions; the least recently used goes first.
  class SessionStore {
  public:
    static constexpr size_t kMaxSessions = 64;
    static constexpr size_t kMaxToplevels = 64;
    // Bounds one save to about kMaxSessions * kMaxToplevels * kMaxNameBytes of names (1 MB).
    static constexpr size_t kMaxNameBytes = 256;

    [[nodiscard]] bool contains(const std::string& id) const { return m_sessions.contains(id); }
    [[nodiscard]] size_t size() const { return m_sessions.size(); }
    [[nodiscard]] const SessionPlacement* find(const std::string& id, const std::string& name) const;
    // Stores the placement and marks the session used; false when nothing changed, the name is longer than kMaxNameBytes or
    // the session holds kMaxToplevels other names.
    bool put(const std::string& id, const std::string& name, const SessionPlacement& placement);
    bool erase(const std::string& id, const std::string& name);
    bool eraseSession(const std::string& id);
    bool rename(const std::string& id, const std::string& from, const std::string& to);
    // Marks a stored session used, so pruning keeps it longer.
    void touch(const std::string& id);

    [[nodiscard]] nlohmann::json toJson() const;
    // Reads a state file; `skipped` counts the sessions and toplevels it ignores as malformed. Never throws on content.
    static SessionStore fromJson(const nlohmann::json& file, size_t& skipped);

  private:
    struct Record {
      uint64_t used = 0;
      std::map<std::string, SessionPlacement> toplevels;
    };
    void prune();

    std::map<std::string, Record> m_sessions;
    uint64_t m_clock = 0;
  };

} // namespace andromeda
