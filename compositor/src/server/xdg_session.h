#pragma once

#include "server/xdg_session_state.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_xdg_toplevel;

namespace andromeda {

  class Server;
  class View;

  // Where `view` is now, in the form a restore resolves: an unnamed workspace by its live position, a named one by name.
  SessionPlacement sessionPlacementOf(const View& view);

  // Serves xdg-session-management-v1 (wayland-protocols staging): an app keeps its windows' placement across app and
  // compositor restarts by a session id. Placements live in `stateFile` (docs/resilience.md §5).
  class XdgSessionManager {
  public:
    XdgSessionManager(Server& server, wl_display* display, std::filesystem::path stateFile);
    ~XdgSessionManager();

    XdgSessionManager(const XdgSessionManager&) = delete;
    XdgSessionManager& operator=(const XdgSessionManager&) = delete;

    // True while a flush has work: a tracked window to record, or a store change not yet written.
    [[nodiscard]] bool wantsFlush() const;
    // Records the placement of every tracked window and writes the store if it changed. Server's debounced session
    // save timer calls it.
    void flush();
    // Records the placement of `toplevel` before its window unmaps.
    void windowUnmapping(wlr_xdg_toplevel* toplevel);
    // The placement restore_toplevel asked for `toplevel`; `consume` drops it once the window maps. The first call
    // sends xdg_toplevel_session_v1.restored.
    std::optional<SessionPlacement> placementFor(wlr_xdg_toplevel* toplevel, bool consume);

  private:
    struct Session;
    struct Toplevel;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleGetSession(wl_client* client, wl_resource* resource, uint32_t id, uint32_t reason, const char* sessionId);
    static void handleAddToplevel(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* toplevel, const char* name);
    static void handleRestoreToplevel(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* toplevel, const char* name);
    static void handleRemoveToplevel(wl_client* client, wl_resource* resource, const char* name);
    static void handleSessionRemove(wl_client* client, wl_resource* resource);
    static void handleSessionResourceDestroy(wl_resource* resource);
    static void handleToplevelRename(wl_client* client, wl_resource* resource, const char* name);
    static void handleToplevelResourceDestroy(wl_resource* resource);
    static void handleToplevelDestroy(wl_listener* listener, void* data);

    static Session* sessionFrom(wl_resource* resource);
    static Toplevel* toplevelFrom(wl_resource* resource);
    // add_toplevel and restore_toplevel; `restore` looks up the stored placement.
    static void addToplevel(wl_resource* resource, uint32_t id, wl_resource* toplevel, const char* name, bool restore);
    // Applies `event` to the entry; false when the transition is invalid. An entry that becomes Free is deleted, and its
    // resource, if any, goes inert.
    static bool step(Toplevel* entry, SessionEntryEvent event);
    // Removes every entry of `session` and deletes it; its resource goes inert.
    static void dropSession(Session* session);
    // Stores where the entry's window is when it is tracked and mapped; true when the store changed.
    bool record(const Session& session, const Toplevel& entry);
    void storeChanged();
    void save();
    void load();

    Server& m_server;
    wl_global* m_global = nullptr;
    std::filesystem::path m_stateFile;
    SessionStore m_store;
    bool m_unsaved = false;
    std::vector<Session*> m_sessions;
  };

} // namespace andromeda
