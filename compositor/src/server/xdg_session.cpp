#include "server/xdg_session.h"

#include "core/async_file_writer.h"
#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/workspace.h"
#include "wlr.h"
#include "xdg-session-management-v1-protocol.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

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
  } // namespace

  SessionPlacement sessionPlacementOf(const View& view) {
    SessionPlacement placement;
    if (const Output* output = view.currentOutput(); output != nullptr) {
      placement.output = output->wlr()->name;
    }
    // Workspace ids count creations, not positions.
    if (const Workspace* workspace = view.workspace(); workspace != nullptr) {
      placement.workspace = workspace->named() ? WorkspaceReference{WorkspaceName{workspace->name()}}
                                               : WorkspaceReference{WorkspaceIndex{workspace->index() + 1}};
    }
    placement.floating = view.floating();
    placement.fullscreen = view.scheduledFullscreen();
    const wlr_box content = view.committedContentBox();
    placement.width = content.width;
    placement.height = content.height;
    return placement;
  }

  // A live xdg_session_v1. Null user data on its resource makes it inert (replaced, removed, or the manager is gone).
  struct XdgSessionManager::Session {
    XdgSessionManager* manager = nullptr;
    wl_resource* resource = nullptr;
    std::string id;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
  };

  // A toplevel in a live session, owned by the session; it lives until its window or its session goes, whether or not
  // the client keeps its xdg_toplevel_session_v1.
  struct XdgSessionManager::Toplevel {
    Session* session = nullptr;
    // Null once the client destroyed the handle.
    wl_resource* resource = nullptr;
    wlr_xdg_toplevel* toplevel = nullptr;
    std::string name;
    SessionEntryState state = SessionEntryState::Free;
    // The stored placement; read only in Restoring and Configured.
    SessionPlacement restore;
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
    while (!m_sessions.empty()) {
      dropSession(m_sessions.back());
    }
    if (m_unsaved) {
      save();
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

  bool XdgSessionManager::step(Toplevel* entry, SessionEntryEvent event) {
    const std::optional<SessionEntryState> next = transitionSessionEntry(entry->state, event);
    if (!next) {
      return false;
    }
    if (sessionEntrySendsRestored(entry->state, *next) && entry->resource != nullptr) {
      xdg_toplevel_session_v1_send_restored(entry->resource);
    }
    entry->state = *next;
    if (event == SessionEntryEvent::ResourceDestroyed) {
      entry->resource = nullptr;
    }
    if (*next == SessionEntryState::Free) {
      wl_list_remove(&entry->toplevelDestroy.link);
      if (entry->resource != nullptr) {
        wl_resource_set_user_data(entry->resource, nullptr);
      }
      std::erase_if(entry->session->toplevels, [entry](const auto& owned) { return owned.get() == entry; });
    }
    return true;
  }

  void XdgSessionManager::dropSession(Session* session) {
    while (!session->toplevels.empty()) {
      step(session->toplevels.back().get(), SessionEntryEvent::Remove);
    }
    wl_resource_set_user_data(session->resource, nullptr);
    std::erase(session->manager->m_sessions, session);
    delete session;
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
    const auto live = std::ranges::find(self->m_sessions, sessionKey, &Session::id);
    if (!sessionKey.empty() && live == self->m_sessions.end() && !self->m_store.contains(sessionKey)) {
      sessionKey.clear();
    }
    if (!sessionKey.empty() && live != self->m_sessions.end()) {
      if (wl_resource_get_client((*live)->resource) == client) {
        wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_IN_USE, "session is in use");
        return;
      }
      // Another client takes the session over; the old object goes inert.
      xdg_session_v1_send_replaced((*live)->resource);
      dropSession(*live);
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
    // A new session reaches the store with its first placement.
    if (sessionKey.empty()) {
      session->id = newSessionId();
      xdg_session_v1_send_created(sessionResource, session->id.c_str());
    } else {
      session->id = sessionKey;
      self->m_store.touch(sessionKey);
      self->storeChanged();
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
      bool changed = false;
      for (const auto& entry : session->toplevels) {
        changed = self->record(*session, *entry) || changed;
      }
      if (changed) {
        self->storeChanged();
      }
    }
    dropSession(session);
  }

  void XdgSessionManager::handleSessionRemove(wl_client*, wl_resource* resource) {
    if (Session* session = sessionFrom(resource)) {
      XdgSessionManager* self = session->manager;
      if (self->m_store.eraseSession(session->id)) {
        self->storeChanged();
      }
      dropSession(session);
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
    if (std::ranges::any_of(session->toplevels, [&](const auto& entry) { return entry->name == entryName; })) {
      wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "toplevel name is in use");
      return;
    }
    for (const Session* other : self->m_sessions) {
      if (wl_resource_get_client(other->resource) == client
          && std::ranges::any_of(other->toplevels, [toplevel](const auto& e) { return e->toplevel == toplevel; })) {
        wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_ALREADY_ADDED, "toplevel already added");
        return;
      }
    }
    if (restore && toplevel->base->initialized) {
      wl_resource_post_error(resource, XDG_SESSION_V1_ERROR_ALREADY_MAPPED, "toplevel was committed before restore");
      return;
    }
    auto entry = std::make_unique<Toplevel>();
    entry->session = session;
    entry->resource = entryResource;
    entry->toplevel = toplevel;
    entry->name = entryName;
    SessionEntryEvent event = SessionEntryEvent::Add;
    if (!restore) {
      if (self->m_store.erase(session->id, entryName)) {
        self->storeChanged();
      }
    } else if (const SessionPlacement* stored = self->m_store.find(session->id, entryName)) {
      entry->restore = *stored;
      event = SessionEntryEvent::Restore;
    } else {
      event = SessionEntryEvent::RestoreUnknown;
    }
    step(entry.get(), event);
    entry->toplevelDestroy.notify = handleToplevelDestroy;
    wl_signal_add(&toplevel->events.destroy, &entry->toplevelDestroy);
    wl_resource_set_user_data(entryResource, entry.get());
    session->toplevels.push_back(std::move(entry));
    // add_toplevel may name a window that is already mapped.
    self->m_server.scheduleSessionSave();
  }

  void XdgSessionManager::handleRemoveToplevel(wl_client*, wl_resource* resource, const char* name) {
    Session* session = sessionFrom(resource);
    if (session == nullptr || name == nullptr) {
      return;
    }
    XdgSessionManager* self = session->manager;
    const std::string entryName(name);
    const auto entry = std::ranges::find(session->toplevels, entryName, [](const auto& e) { return e->name; });
    if (entry != session->toplevels.end()) {
      step(entry->get(), SessionEntryEvent::Remove);
    }
    if (self->m_store.erase(session->id, entryName)) {
      self->storeChanged();
    }
  }

  void XdgSessionManager::handleToplevelRename(wl_client*, wl_resource* resource, const char* name) {
    Toplevel* entry = toplevelFrom(resource);
    if (entry == nullptr || name == nullptr) {
      return;
    }
    Session* session = entry->session;
    const std::string newName(name);
    if (newName == entry->name) {
      return;
    }
    if (std::ranges::any_of(session->toplevels, [&](const auto& other) { return other->name == newName; })) {
      wl_resource_post_error(session->resource, XDG_SESSION_V1_ERROR_NAME_IN_USE, "toplevel name is in use");
      return;
    }
    if (session->manager->m_store.rename(session->id, entry->name, newName)) {
      session->manager->storeChanged();
    }
    entry->name = newName;
  }

  void XdgSessionManager::handleToplevelResourceDestroy(wl_resource* resource) {
    if (Toplevel* entry = toplevelFrom(resource)) {
      step(entry, SessionEntryEvent::ResourceDestroyed);
    }
  }

  void XdgSessionManager::handleToplevelDestroy(wl_listener* listener, void* /*data*/) {
    Toplevel* entry = wl_container_of(listener, entry, toplevelDestroy);
    step(entry, SessionEntryEvent::ToplevelDestroyed);
  }

  bool XdgSessionManager::record(const Session& session, const Toplevel& entry) {
    if (entry.state != SessionEntryState::Tracking) {
      return false;
    }
    const View* view = View::fromSurface(entry.toplevel->base->surface);
    if (view == nullptr || !view->mapped()) {
      return false;
    }
    return m_store.put(session.id, entry.name, sessionPlacementOf(*view));
  }

  void XdgSessionManager::storeChanged() {
    m_unsaved = true;
    m_server.scheduleSessionSave();
  }

  bool XdgSessionManager::wantsFlush() const {
    return m_unsaved || std::ranges::any_of(m_sessions, [](const Session* session) {
             return std::ranges::any_of(session->toplevels, [](const auto& entry) {
               return entry->state == SessionEntryState::Tracking;
             });
           });
  }

  void XdgSessionManager::flush() {
    for (const Session* session : m_sessions) {
      for (const auto& entry : session->toplevels) {
        m_unsaved = record(*session, *entry) || m_unsaved;
      }
    }
    if (m_unsaved) {
      save();
    }
  }

  void XdgSessionManager::windowUnmapping(wlr_xdg_toplevel* toplevel) {
    if (m_server.stopping()) {
      return;
    }
    for (const Session* session : m_sessions) {
      for (const auto& entry : session->toplevels) {
        if (entry->toplevel == toplevel && record(*session, *entry)) {
          storeChanged();
        }
      }
    }
  }

  std::optional<SessionPlacement> XdgSessionManager::placementFor(wlr_xdg_toplevel* toplevel, bool consume) {
    for (Session* session : m_sessions) {
      for (const auto& entry : session->toplevels) {
        if (entry->toplevel != toplevel) {
          continue;
        }
        if (!step(entry.get(), consume ? SessionEntryEvent::Map : SessionEntryEvent::Configure)) {
          return std::nullopt;
        }
        return entry->restore;
      }
    }
    return std::nullopt;
  }

  void XdgSessionManager::save() {
    m_unsaved = false;
    if (!m_stateFile.empty()) {
      writeFileAsync(m_stateFile, m_store.toJson().dump());
    }
  }

  void XdgSessionManager::load() {
    if (m_stateFile.empty()) {
      return;
    }
    std::ifstream in(m_stateFile);
    if (!in) {
      return;
    }
    size_t skipped = 0;
    m_store = SessionStore::fromJson(nlohmann::json::parse(in, nullptr, false), skipped);
    if (skipped > 0) {
      kLog.warn("ignoring {} malformed entries in {}", skipped, m_stateFile.string());
    }
  }

} // namespace andromeda
