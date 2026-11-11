#include "server/xdg_session.h"

#include "config/config.h"
#include "core/async_file_writer.h"
#include "core/log.h"
#include "server/ipc_commands.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/workspace.h"
#include "wlr.h"
#include "xdg-session-management-v1-protocol.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <ranges>

namespace andromeda {

  namespace {
    constexpr Logger kLog("xdg-session");
    constexpr int kVersion = 1;

    std::string newSessionId() {
      std::random_device random;
      std::string id;
      for (int i = 0; i < 4; ++i) {
        id += std::format("{:08x}", random());
      }
      return id;
    }

    nlohmann::json placementJson(const SessionPlacement& placement) {
      return {
          {"output", placement.output},       {"workspace", placement.workspace}, {"floating", placement.floating},
          {"fullscreen", placement.fullscreen}, {"w", placement.width},          {"h", placement.height},
      };
    }

    SessionPlacement placementFrom(const nlohmann::json& window) {
      return {
          .output = window.value("output", ""),
          .workspace = window.value("workspace", ""),
          .floating = window.value("floating", false),
          .fullscreen = window.value("fullscreen", false),
          .width = window.value("w", 0),
          .height = window.value("h", 0),
      };
    }
  } // namespace

  void applySessionPlacement(const SessionPlacement& placement, ResolvedWindowRule& rule) {
    if (!placement.output.empty()) {
      rule.defaultOutput = placement.output;
    }
    // Workspace ids are "OUTPUT:NAME"; a numeric name is a position.
    const std::string name = placement.workspace.substr(placement.workspace.rfind(':') + 1);
    if (!name.empty()) {
      size_t position = 0;
      const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), position);
      if (error == std::errc{} && end == name.data() + name.size() && position > 0) {
        rule.defaultWorkspace = WorkspaceIndex{position};
      } else {
        rule.defaultWorkspace = WorkspaceName{name};
      }
    }
    rule.defaultFloating = placement.floating;
    rule.defaultFullscreen = placement.fullscreen;
    if (placement.floating && placement.width > 0 && placement.height > 0) {
      rule.defaultFloatingWidthPx = placement.width;
      rule.defaultFloatingHeightPx = placement.height;
    }
  }

  // A live xdg_session_v1. Null user data on its resource makes it inert (replaced, removed, or the manager is gone).
  struct XdgSessionManager::Session {
    XdgSessionManager* manager = nullptr;
    wl_resource* resource = nullptr;
    std::string id;
    std::vector<Toplevel*> toplevels;
  };

  // A live xdg_toplevel_session_v1, owned by its resource; session and toplevel are null once detached (inert).
  struct XdgSessionManager::Toplevel {
    Session* session = nullptr;
    wl_resource* resource = nullptr;
    wlr_xdg_toplevel* toplevel = nullptr;
    std::string name;
    std::optional<SessionPlacement> pending;
    bool restoredSent = false;
    wl_listener toplevelDestroy{};
  };

  XdgSessionManager::XdgSessionManager(Server& server, wl_display* display, std::filesystem::path stateFile)
      : m_server(server), m_stateFile(std::move(stateFile)) {
    load();
    m_global = wl_global_create(display, &xdg_session_manager_v1_interface, kVersion, this, bind);
    if (m_global == nullptr) {
      kLog.error("failed to create the xdg_session_manager_v1 global");
    }
  }

  XdgSessionManager::~XdgSessionManager() {
    if (m_snapshotIdle != nullptr) {
      wl_event_source_remove(m_snapshotIdle);
    }
    for (Session* session : m_sessions) {
      for (Toplevel* entry : std::vector(session->toplevels)) {
        detach(entry);
      }
      wl_resource_set_user_data(session->resource, nullptr);
      delete session;
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void XdgSessionManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    static const struct xdg_session_manager_v1_interface implementation = {
        .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
        .get_session = handleGetSession,
    };
    wl_resource* resource = wl_resource_create(client, &xdg_session_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &implementation, data, nullptr);
  }

  XdgSessionManager::Session* XdgSessionManager::sessionFrom(wl_resource* resource) {
    return static_cast<Session*>(wl_resource_get_user_data(resource));
  }

  XdgSessionManager::Toplevel* XdgSessionManager::toplevelFrom(wl_resource* resource) {
    return static_cast<Toplevel*>(wl_resource_get_user_data(resource));
  }

  void XdgSessionManager::handleGetSession(
      wl_client* client, wl_resource* resource, uint32_t id, uint32_t reason, const char* sessionId
  ) {
    static const struct xdg_session_v1_interface implementation = {
        .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
        .remove = handleSessionRemove,
        .add_toplevel = handleAddToplevel,
        .restore_toplevel = handleRestoreToplevel,
        .remove_toplevel = handleRemoveToplevel,
    };
    auto* self = static_cast<XdgSessionManager*>(wl_resource_get_user_data(resource));
    if (reason < XDG_SESSION_MANAGER_V1_REASON_LAUNCH || reason > XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE) {
      wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON, "invalid reason %u", reason);
      return;
    }
    std::string sessionKey = sessionId != nullptr ? sessionId : "";
    if (!sessionKey.empty() && !self->m_store.contains(sessionKey)) {
      sessionKey.clear();
    }
    if (!sessionKey.empty()) {
      const auto live = std::ranges::find(self->m_sessions, sessionKey, &Session::id);
      if (live != self->m_sessions.end()) {
        if (wl_resource_get_client((*live)->resource) == client) {
          wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_IN_USE, "session is in use");
          return;
        }
        // Another client takes the session over; the old object goes inert.
        Session* old = *live;
        xdg_session_v1_send_replaced(old->resource);
        for (Toplevel* entry : std::vector(old->toplevels)) {
          detach(entry);
        }
        wl_resource_set_user_data(old->resource, nullptr);
        self->m_sessions.erase(live);
        delete old;
      }
    }
    wl_resource* sessionResource =
        wl_resource_create(client, &xdg_session_v1_interface, wl_resource_get_version(resource), id);
    if (sessionResource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    auto* session = new Session{.manager = self, .resource = sessionResource, .id = {}, .toplevels = {}};
    wl_resource_set_implementation(sessionResource, &implementation, session, handleSessionResourceDestroy);
    self->m_sessions.push_back(session);
    if (sessionKey.empty()) {
      session->id = newSessionId();
      self->m_store[session->id];
      self->save();
      xdg_session_v1_send_created(sessionResource, session->id.c_str());
    } else {
      session->id = sessionKey;
      xdg_session_v1_send_restored(sessionResource);
    }
  }

  void XdgSessionManager::handleSessionResourceDestroy(wl_resource* resource) {
    Session* session = sessionFrom(resource);
    if (session == nullptr) {
      return;
    }
    XdgSessionManager* self = session->manager;
    // destroy keeps the state; take the latest placements first, unless the server is tearing down.
    if (!self->m_server.stopping()) {
      self->snapshot();
    }
    for (Toplevel* entry : std::vector(session->toplevels)) {
      detach(entry);
    }
    std::erase(self->m_sessions, session);
    delete session;
  }

  void XdgSessionManager::handleSessionRemove(wl_client*, wl_resource* resource) {
    if (Session* session = sessionFrom(resource)) {
      XdgSessionManager* self = session->manager;
      self->m_store.erase(session->id);
      for (Toplevel* entry : std::vector(session->toplevels)) {
        detach(entry);
      }
      std::erase(self->m_sessions, session);
      wl_resource_set_user_data(resource, nullptr);
      delete session;
      self->save();
    }
    wl_resource_destroy(resource);
  }

  void XdgSessionManager::handleAddToplevel(
      wl_client*, wl_resource* resource, uint32_t id, wl_resource* toplevel, const char* name
  ) {
    addToplevel(resource, id, toplevel, name, false);
  }

  void XdgSessionManager::handleRestoreToplevel(
      wl_client*, wl_resource* resource, uint32_t id, wl_resource* toplevel, const char* name
  ) {
    addToplevel(resource, id, toplevel, name, true);
  }

  void XdgSessionManager::addToplevel(
      wl_resource* resource, uint32_t id, wl_resource* toplevelResource, const char* name, bool restore
  ) {
    static const struct xdg_toplevel_session_v1_interface implementation = {
        .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
        .rename = handleToplevelRename,
    };
    wl_client* client = wl_resource_get_client(resource);
    wl_resource* entryResource =
        wl_resource_create(client, &xdg_toplevel_session_v1_interface, wl_resource_get_version(resource), id);
    if (entryResource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(entryResource, &implementation, nullptr, handleToplevelResourceDestroy);
    Session* session = sessionFrom(resource);
    if (session == nullptr) {
      return;
    }
    XdgSessionManager* self = session->manager;
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevelResource);
    const std::string entryName = name != nullptr ? name : "";
    if (toplevel == nullptr) {
      return;
    }
    if (std::ranges::any_of(session->toplevels, [&](const Toplevel* entry) { return entry->name == entryName; })) {
      wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "toplevel name is in use");
      return;
    }
    for (const Session* other : self->m_sessions) {
      if (wl_resource_get_client(other->resource) == client
          && std::ranges::any_of(other->toplevels, [toplevel](const Toplevel* e) { return e->toplevel == toplevel; })) {
        wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_ALREADY_ADDED, "toplevel already added");
        return;
      }
    }
    if (restore && toplevel->base->initialized) {
      wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_ALREADY_MAPPED, "toplevel was committed before restore");
      return;
    }
    auto* entry = new Toplevel{};
    entry->session = session;
    entry->resource = entryResource;
    entry->toplevel = toplevel;
    entry->name = entryName;
    std::map<std::string, SessionPlacement>& stored = self->m_store[session->id];
    if (restore) {
      if (const auto found = stored.find(entryName); found != stored.end()) {
        entry->pending = found->second;
      }
    } else if (stored.erase(entryName) > 0) {
      self->save();
    }
    entry->toplevelDestroy.notify = handleToplevelDestroy;
    wl_signal_add(&toplevel->events.destroy, &entry->toplevelDestroy);
    wl_resource_set_user_data(entryResource, entry);
    session->toplevels.push_back(entry);
    self->scheduleSnapshot();
  }

  void XdgSessionManager::handleRemoveToplevel(wl_client*, wl_resource* resource, const char* name) {
    Session* session = sessionFrom(resource);
    if (session == nullptr || name == nullptr) {
      return;
    }
    XdgSessionManager* self = session->manager;
    const std::string entryName(name);
    for (Toplevel* entry : std::vector(session->toplevels)) {
      if (entry->name == entryName) {
        detach(entry);
      }
    }
    if (self->m_store[session->id].erase(entryName) > 0) {
      self->save();
    }
  }

  void XdgSessionManager::handleToplevelRename(wl_client*, wl_resource* resource, const char* name) {
    Toplevel* entry = toplevelFrom(resource);
    if (entry == nullptr || entry->session == nullptr || name == nullptr) {
      return;
    }
    Session* session = entry->session;
    const std::string newName(name);
    if (newName == entry->name) {
      return;
    }
    if (std::ranges::any_of(session->toplevels, [&](const Toplevel* other) { return other->name == newName; })) {
      wl_resource_post_error(session->resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "toplevel name is in use");
      return;
    }
    std::map<std::string, SessionPlacement>& stored = session->manager->m_store[session->id];
    if (auto node = stored.extract(entry->name); !node.empty()) {
      node.key() = newName;
      stored.insert_or_assign(newName, std::move(node.mapped()));
      session->manager->save();
    }
    entry->name = newName;
  }

  void XdgSessionManager::handleToplevelResourceDestroy(wl_resource* resource) {
    Toplevel* entry = toplevelFrom(resource);
    if (entry == nullptr) {
      return;
    }
    detach(entry);
    delete entry;
  }

  void XdgSessionManager::handleToplevelDestroy(wl_listener* listener, void* /*data*/) {
    Toplevel* entry = wl_container_of(listener, entry, toplevelDestroy);
    wl_list_remove(&entry->toplevelDestroy.link);
    wl_list_init(&entry->toplevelDestroy.link);
    entry->toplevel = nullptr;
  }

  void XdgSessionManager::detach(Toplevel* entry) {
    if (entry->session != nullptr) {
      std::erase(entry->session->toplevels, entry);
      entry->session = nullptr;
    }
    if (entry->toplevel != nullptr) {
      wl_list_remove(&entry->toplevelDestroy.link);
      wl_list_init(&entry->toplevelDestroy.link);
      entry->toplevel = nullptr;
    }
    entry->pending.reset();
  }

  void XdgSessionManager::scheduleSnapshot() {
    if (m_sessions.empty() || m_snapshotIdle != nullptr) {
      return;
    }
    m_snapshotIdle = wl_event_loop_add_idle(
        wl_display_get_event_loop(m_server.display()),
        [](void* data) {
          auto* self = static_cast<XdgSessionManager*>(data);
          self->m_snapshotIdle = nullptr;
          self->snapshot();
        },
        this
    );
  }

  void XdgSessionManager::snapshot() {
    const auto live = [](const Toplevel* entry) { return entry->toplevel != nullptr; };
    if (!std::ranges::any_of(m_sessions, [&](const Session* s) { return std::ranges::any_of(s->toplevels, live); })) {
      return;
    }
    const nlohmann::json windows = IpcCommands::windows(m_server, "")["ok"];
    bool changed = false;
    for (const Session* session : m_sessions) {
      for (const Toplevel* entry : session->toplevels | std::views::filter(live)) {
        const View* view = View::fromSurface(entry->toplevel->base->surface);
        const char* windowId = view != nullptr && view->mapped() ? view->extForeignIdentifier() : nullptr;
        if (windowId == nullptr) {
          continue;
        }
        const auto window = std::ranges::find_if(windows, [windowId](const nlohmann::json& w) {
          return w.value("id", "") == windowId;
        });
        if (window == windows.end()) {
          continue;
        }
        SessionPlacement placement = placementFrom(*window);
        // Workspace ids count creations, not positions; store the position (or the name) that a restore resolves.
        if (const Workspace* workspace = view->workspace(); workspace != nullptr) {
          placement.workspace = std::format(
              "{}:{}", placement.output, workspace->named() ? workspace->name() : std::to_string(workspace->index() + 1)
          );
        }
        SessionPlacement& stored = m_store[session->id][entry->name];
        if (stored != placement) {
          stored = placement;
          changed = true;
        }
      }
    }
    if (changed) {
      save();
    }
  }

  std::optional<SessionPlacement> XdgSessionManager::placementFor(wlr_xdg_toplevel* toplevel, bool consume) {
    for (Session* session : m_sessions) {
      for (Toplevel* entry : session->toplevels) {
        if (entry->toplevel != toplevel || !entry->pending) {
          continue;
        }
        if (!std::exchange(entry->restoredSent, true)) {
          xdg_toplevel_session_v1_send_restored(entry->resource);
        }
        std::optional<SessionPlacement> placement = entry->pending;
        if (consume) {
          entry->pending.reset();
        }
        return placement;
      }
    }
    return std::nullopt;
  }

  void XdgSessionManager::save() const {
    if (m_stateFile.empty()) {
      return;
    }
    nlohmann::json sessions = nlohmann::json::object();
    for (const auto& [id, toplevels] : m_store) {
      nlohmann::json entries = nlohmann::json::object();
      for (const auto& [name, placement] : toplevels) {
        entries[name] = placementJson(placement);
      }
      sessions[id] = std::move(entries);
    }
    writeFileAsync(m_stateFile, nlohmann::json{{"version", 1}, {"sessions", sessions}}.dump());
  }

  void XdgSessionManager::load() {
    if (m_stateFile.empty()) {
      return;
    }
    std::ifstream in(m_stateFile);
    if (!in) {
      return;
    }
    const nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
    if (file.is_discarded() || !file.contains("sessions") || !file["sessions"].is_object()) {
      kLog.warn("ignoring unreadable {}", m_stateFile.string());
      return;
    }
    for (const auto& [id, toplevels] : file["sessions"].items()) {
      std::map<std::string, SessionPlacement>& stored = m_store[id];
      if (!toplevels.is_object()) {
        continue;
      }
      for (const auto& [name, placement] : toplevels.items()) {
        if (placement.is_object()) {
          stored[name] = placementFrom(placement);
        }
      }
    }
  }

} // namespace andromeda
