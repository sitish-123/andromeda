#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <wayland-server-core.h>

struct wlr_data_source;
struct wlr_drag;
struct xdg_toplevel_drag_manager_v1_interface;
struct xdg_toplevel_drag_v1_interface;

namespace umbriel {

  class Server;
  class View;

  // Serves xdg_toplevel_drag_manager_v1: while a data-device drag whose source carries a toplevel drag runs, the
  // attached window floats under the pointer at the client's offset, and it takes no part in picking the drop target.
  class ToplevelDragManager {
  public:
    explicit ToplevelDragManager(Server& server);
    ~ToplevelDragManager();

    ToplevelDragManager(const ToplevelDragManager&) = delete;
    ToplevelDragManager& operator=(const ToplevelDragManager&) = delete;

    void dragStarted(wlr_drag* drag);
    void pointerMoved(double x, double y);
    // The mapped window a running drag carries, which hit-testing skips.
    [[nodiscard]] View* draggedView() const;

  private:
    struct Drag {
      ToplevelDragManager* manager = nullptr;
      wl_resource* resource = nullptr;
      wlr_data_source* source = nullptr;
      wlr_drag* drag = nullptr;
      View* view = nullptr;
      int offsetX = 0;
      int offsetY = 0;
      wl_listener sourceDestroy{};
      wl_listener dragDestroy{};
      wl_listener viewMap{};
      wl_listener viewUnmap{};
      wl_listener viewDestroy{};
    };

    // The scanner also declares wl_interface variables of these names, so the structs need their elaborated form.
    static const struct xdg_toplevel_drag_manager_v1_interface kManagerImplementation;
    static const struct xdg_toplevel_drag_v1_interface kDragImplementation;

    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void handleManagerDestroy(wl_client* client, wl_resource* resource);
    static void handleGetDrag(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* sourceResource);
    static void handleAttach(wl_client* client, wl_resource* resource, wl_resource* toplevel, int32_t x, int32_t y);
    static void handleDragDestroyRequest(wl_client* client, wl_resource* resource);
    static void handleResourceDestroyed(wl_resource* resource);
    static void handleSourceDestroy(wl_listener* listener, void* data);
    static void handleDragDestroy(wl_listener* listener, void* data);
    static void handleViewMap(wl_listener* listener, void* data);
    static void handleViewUnmap(wl_listener* listener, void* data);
    static void handleViewDestroy(wl_listener* listener, void* data);

    static Drag* dragFrom(wl_resource* resource);
    static void detach(Drag& drag);
    void follow(Drag& drag, double x, double y);
    void dropped(Drag& drag);
    [[nodiscard]] Drag* running() const;

    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<std::unique_ptr<Drag>> m_drags;
  };

} // namespace umbriel
