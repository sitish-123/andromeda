#include "server/xdg_session_state.h"

#include "config/config.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace andromeda {

  namespace {
    constexpr int kFileVersion = 2;

    // Reads `key` into `out` when present; false when it is present with another type.
    template <typename T> bool readField(const nlohmann::json& object, const char* key, T& out) {
      const auto it = object.find(key);
      if (it == object.end()) {
        return true;
      }
      if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
          return false;
        }
      } else if constexpr (std::is_same_v<T, int>) {
        if (!it->is_number_integer()) {
          return false;
        }
      } else {
        if (!it->is_string()) {
          return false;
        }
      }
      out = it->template get<T>();
      return true;
    }
  } // namespace

  void applySessionPlacement(const SessionPlacement& placement, ResolvedWindowRule& rule) {
    if (!placement.output.empty()) {
      rule.defaultOutput = placement.output;
    }
    if (placement.workspace) {
      rule.defaultWorkspace = *placement.workspace;
    }
    rule.defaultFloating = placement.floating;
    rule.defaultFullscreen = placement.fullscreen;
    if (placement.floating && placement.width > 0 && placement.height > 0) {
      rule.defaultFloatingWidthPx = placement.width;
      rule.defaultFloatingHeightPx = placement.height;
    }
  }

  nlohmann::json workspaceReferenceJson(const WorkspaceReference& reference) {
    if (const auto* position = std::get_if<WorkspaceIndex>(&reference)) {
      return position->value;
    }
    return std::get<WorkspaceName>(reference).value;
  }

  std::optional<WorkspaceReference> workspaceReferenceFrom(const nlohmann::json& value) {
    if (value.is_number_unsigned() && value.get<uint64_t>() > 0) {
      return WorkspaceIndex{value.get<size_t>()};
    }
    if (value.is_string() && !value.get_ref<const std::string&>().empty()) {
      return WorkspaceName{value.get<std::string>()};
    }
    return std::nullopt;
  }

  nlohmann::json sessionPlacementJson(const SessionPlacement& placement) {
    nlohmann::json json = {
        {"output", placement.output},       {"floating", placement.floating}, {"fullscreen", placement.fullscreen},
        {"w", placement.width},             {"h", placement.height},
    };
    if (placement.workspace) {
      json["workspace"] = workspaceReferenceJson(*placement.workspace);
    }
    return json;
  }

  std::optional<SessionPlacement> sessionPlacementFrom(const nlohmann::json& value) {
    if (!value.is_object()) {
      return std::nullopt;
    }
    SessionPlacement placement;
    if (!readField(value, "output", placement.output) || !readField(value, "floating", placement.floating)
        || !readField(value, "fullscreen", placement.fullscreen) || !readField(value, "w", placement.width)
        || !readField(value, "h", placement.height)) {
      return std::nullopt;
    }
    if (const auto workspace = value.find("workspace"); workspace != value.end()) {
      placement.workspace = workspaceReferenceFrom(*workspace);
      if (!placement.workspace) {
        return std::nullopt;
      }
    }
    return placement;
  }

  std::optional<SessionEntryState> transitionSessionEntry(SessionEntryState from, SessionEntryEvent event) {
    using S = SessionEntryState;
    using E = SessionEntryEvent;
    if (from == S::Free) {
      switch (event) {
      case E::Add:
      case E::RestoreUnknown:
        return S::Tracking;
      case E::Restore:
        return S::Restoring;
      default:
        return std::nullopt;
      }
    }
    switch (event) {
    case E::Add:
    case E::Restore:
    case E::RestoreUnknown:
      return std::nullopt;
    case E::Configure:
      return from == S::Tracking ? std::nullopt : std::optional(S::Configured);
    case E::Map:
      return from == S::Tracking ? std::nullopt : std::optional(S::Tracking);
    case E::ResourceDestroyed:
      return from;
    case E::ToplevelDestroyed:
    case E::Remove:
      return S::Free;
    }
    return std::nullopt;
  }

  const SessionPlacement* SessionStore::find(const std::string& id, const std::string& name) const {
    const auto session = m_sessions.find(id);
    if (session == m_sessions.end()) {
      return nullptr;
    }
    const auto toplevel = session->second.toplevels.find(name);
    return toplevel != session->second.toplevels.end() ? &toplevel->second : nullptr;
  }

  bool SessionStore::put(const std::string& id, const std::string& name, const SessionPlacement& placement) {
    if (name.size() > kMaxNameBytes) {
      return false;
    }
    Record& record = m_sessions[id];
    const auto existing = record.toplevels.find(name);
    if (existing == record.toplevels.end() && record.toplevels.size() >= kMaxToplevels) {
      if (record.toplevels.empty()) {
        m_sessions.erase(id);
      }
      return false;
    }
    if (existing != record.toplevels.end() && existing->second == placement) {
      return false;
    }
    record.toplevels.insert_or_assign(name, placement);
    record.used = ++m_clock;
    prune();
    return true;
  }

  bool SessionStore::erase(const std::string& id, const std::string& name) {
    const auto session = m_sessions.find(id);
    if (session == m_sessions.end() || session->second.toplevels.erase(name) == 0) {
      return false;
    }
    if (session->second.toplevels.empty()) {
      m_sessions.erase(session);
    }
    return true;
  }

  bool SessionStore::eraseSession(const std::string& id) { return m_sessions.erase(id) > 0; }

  bool SessionStore::rename(const std::string& id, const std::string& from, const std::string& to) {
    const auto session = m_sessions.find(id);
    if (session == m_sessions.end()) {
      return false;
    }
    auto node = session->second.toplevels.extract(from);
    if (node.empty()) {
      return false;
    }
    node.key() = to;
    session->second.toplevels.insert_or_assign(to, std::move(node.mapped()));
    return true;
  }

  void SessionStore::touch(const std::string& id) {
    if (const auto session = m_sessions.find(id); session != m_sessions.end()) {
      session->second.used = ++m_clock;
    }
  }

  void SessionStore::prune() {
    while (m_sessions.size() > kMaxSessions) {
      m_sessions.erase(std::ranges::min_element(m_sessions, {}, [](const auto& entry) { return entry.second.used; }));
    }
  }

  nlohmann::json SessionStore::toJson() const {
    nlohmann::json sessions = nlohmann::json::object();
    for (const auto& [id, record] : m_sessions) {
      nlohmann::json toplevels = nlohmann::json::object();
      for (const auto& [name, placement] : record.toplevels) {
        toplevels[name] = sessionPlacementJson(placement);
      }
      sessions[id] = {{"used", record.used}, {"toplevels", std::move(toplevels)}};
    }
    return {{"version", kFileVersion}, {"sessions", std::move(sessions)}};
  }

  SessionStore SessionStore::fromJson(const nlohmann::json& file, size_t& skipped) {
    SessionStore store;
    skipped = 0;
    const auto version = file.is_object() ? file.find("version") : file.end();
    const auto sessions = file.is_object() ? file.find("sessions") : file.end();
    if (version == file.end() || *version != kFileVersion || sessions == file.end() || !sessions->is_object()) {
      skipped = 1;
      return store;
    }
    for (const auto& [id, session] : sessions->items()) {
      const auto toplevels = session.is_object() ? session.find("toplevels") : session.end();
      const auto used = session.is_object() ? session.find("used") : session.end();
      if (id.empty() || toplevels == session.end() || !toplevels->is_object()
          || (used != session.end() && !used->is_number_unsigned())) {
        ++skipped;
        continue;
      }
      Record record;
      record.used = used != session.end() ? used->get<uint64_t>() : 0;
      for (const auto& [name, value] : toplevels->items()) {
        if (record.toplevels.size() >= kMaxToplevels) {
          ++skipped;
          break;
        }
        if (const std::optional<SessionPlacement> placement = sessionPlacementFrom(value)) {
          record.toplevels.emplace(name, *placement);
        } else {
          ++skipped;
        }
      }
      if (!record.toplevels.empty()) {
        store.m_clock = std::max(store.m_clock, record.used);
        store.m_sessions.emplace(id, std::move(record));
      }
    }
    store.prune();
    return store;
  }

} // namespace andromeda
