#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_xdg_toplevel;

namespace andromeda {

  class Server;
  struct ResolvedWindowRule;

  // Where a window was: its output, workspace id ("OUTPUT:NAME"), float or tile, fullscreen, and floating size.
  struct SessionPlacement {
    std::string output;
    std::string workspace;
    bool floating = false;
    bool fullscreen = false;
    int width = 0;
    int height = 0;
    bool operator==(const SessionPlacement&) const = default;
  };

  // Makes the next window that `rule` opens follow `placement`.
  void applySessionPlacement(const SessionPlacement& placement, ResolvedWindowRule& rule);

  // Serves xdg-session-management-v1 (wayland-protocols staging): an app keeps its windows' placement across app and
  // compositor restarts by a session id. Placements live in `stateFile` (docs/resilience.md §5).
  class XdgSessionManager {
  public:
    XdgSessionManager(Server& server, wl_display* display, std::filesystem::path stateFile);
    ~XdgSessionManager();

    XdgSessionManager(const XdgSessionManager&) = delete;
    XdgSessionManager& operator=(const XdgSessionManager&) = delete;

    // Records the placement of every window in a live session on the next idle; call it when a window changes.
    void scheduleSnapshot();
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
    // Detaches the entry from its session and its xdg_toplevel; its resource stays, inert, and owns it.
    static void detach(Toplevel* entry);
    void snapshot();
    void save() const;
    void load();

    Server& m_server;
    wl_global* m_global = nullptr;
    std::filesystem::path m_stateFile;
    // session id -> toplevel name -> placement
    std::map<std::string, std::map<std::string, SessionPlacement>> m_store;
    std::vector<Session*> m_sessions;
    wl_event_source* m_snapshotIdle = nullptr;
  };

} // namespace andromeda
