#include "server/toplevel_drag.h"

#include "input/cursor.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"
#include "xdg-toplevel-drag-v1-protocol.h"

#include <algorithm>
#include <cmath>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    // wlroots keeps a client data source's wlr_data_source as the first member of the resource's user data.
    wlr_data_source* dataSourceFrom(wl_resource* resource) {
      return static_cast<wlr_data_source*>(wl_resource_get_user_data(resource));
    }

    void removeListener(wl_listener& listener) {
      if (listener.link.next != nullptr) {
        wl_list_remove(&listener.link);
        listener.link = {};
      }
    }

  } // namespace

  ToplevelDragManager::ToplevelDragManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &xdg_toplevel_drag_manager_v1_interface, kVersion, this, bind);
  }

  ToplevelDragManager::~ToplevelDragManager() {
    for (const auto& drag : m_drags) {
      detach(*drag);
      removeListener(drag->sourceDestroy);
      removeListener(drag->dragDestroy);
      wl_resource_set_user_data(drag->resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void ToplevelDragManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource =
        wl_resource_create(client, &xdg_toplevel_drag_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kManagerImplementation, data, nullptr);
  }

  void ToplevelDragManager::handleManagerDestroy(wl_client* /*client*/, wl_resource* resource) {
    wl_resource_destroy(resource);
  }

  void ToplevelDragManager::handleGetDrag(
      wl_client* client, wl_resource* resource, uint32_t id, wl_resource* sourceResource
  ) {
    auto* self = static_cast<ToplevelDragManager*>(wl_resource_get_user_data(resource));
    wlr_data_source* source = dataSourceFrom(sourceResource);
    const bool used = std::ranges::any_of(self->m_drags, [source](const auto& drag) { return drag->source == source; });
    if (source == nullptr || used) {
      wl_resource_post_error(
          resource, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE, "the data source already has a toplevel drag"
      );
      return;
    }
    wl_resource* dragResource =
        wl_resource_create(client, &xdg_toplevel_drag_v1_interface, wl_resource_get_version(resource), id);
    if (dragResource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    auto drag = std::make_unique<Drag>();
    drag->manager = self;
    drag->resource = dragResource;
    drag->source = source;
    drag->sourceDestroy.notify = handleSourceDestroy;
    wl_signal_add(&source->events.destroy, &drag->sourceDestroy);
    wl_resource_set_implementation(dragResource, &kDragImplementation, drag.get(), handleResourceDestroyed);
    self->m_drags.push_back(std::move(drag));
  }

  ToplevelDragManager::Drag* ToplevelDragManager::dragFrom(wl_resource* resource) {
    return static_cast<Drag*>(wl_resource_get_user_data(resource));
  }

  void ToplevelDragManager::handleAttach(
      wl_client* /*client*/, wl_resource* resource, wl_resource* toplevelResource, int32_t x, int32_t y
  ) {
    Drag* drag = dragFrom(resource);
    if (drag == nullptr) {
      return;
    }
    if (drag->view != nullptr && drag->view->mapped()) {
      wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED, "a mapped toplevel is attached");
      return;
    }
    detach(*drag);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevelResource);
    const auto views = drag->manager->m_server.registry().all();
    const auto found =
        std::ranges::find_if(views, [toplevel](const auto& view) { return view->toplevel() == toplevel; });
    if (found == views.end()) {
      return;
    }
    drag->view = found->get();
    drag->offsetX = x;
    drag->offsetY = y;
    drag->viewMap.notify = handleViewMap;
    wl_signal_add(&toplevel->base->surface->events.map, &drag->viewMap);
    drag->viewUnmap.notify = handleViewUnmap;
    wl_signal_add(&toplevel->base->surface->events.unmap, &drag->viewUnmap);
    drag->viewDestroy.notify = handleViewDestroy;
    wl_signal_add(&toplevel->events.destroy, &drag->viewDestroy);
    if (drag->drag != nullptr && drag->view->mapped()) {
      const wlr_cursor* cursor = drag->manager->m_server.cursor()->wlr();
      drag->manager->follow(*drag, cursor->x, cursor->y);
    }
  }

  void ToplevelDragManager::handleDragDestroyRequest(wl_client* /*client*/, wl_resource* resource) {
    if (const Drag* drag = dragFrom(resource); drag != nullptr && drag->drag != nullptr) {
      wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG, "the drag has not ended");
      return;
    }
    wl_resource_destroy(resource);
  }

  void ToplevelDragManager::handleResourceDestroyed(wl_resource* resource) {
    Drag* drag = dragFrom(resource);
    if (drag == nullptr) {
      return;
    }
    detach(*drag);
    removeListener(drag->sourceDestroy);
    removeListener(drag->dragDestroy);
    std::erase_if(drag->manager->m_drags, [drag](const auto& owned) { return owned.get() == drag; });
  }

  void ToplevelDragManager::handleSourceDestroy(wl_listener* listener, void* /*data*/) {
    Drag* drag = wl_container_of(listener, drag, sourceDestroy);
    removeListener(drag->sourceDestroy);
    drag->source = nullptr;
  }

  void ToplevelDragManager::handleDragDestroy(wl_listener* listener, void* /*data*/) {
    Drag* drag = wl_container_of(listener, drag, dragDestroy);
    removeListener(drag->dragDestroy);
    drag->drag = nullptr;
    drag->manager->dropped(*drag);
  }

  // Runs after the view's own map listener, registered first, has placed the window.
  void ToplevelDragManager::handleViewMap(wl_listener* listener, void* /*data*/) {
    Drag* drag = wl_container_of(listener, drag, viewMap);
    if (drag->drag != nullptr) {
      const wlr_cursor* cursor = drag->manager->m_server.cursor()->wlr();
      drag->manager->follow(*drag, cursor->x, cursor->y);
    }
  }

  void ToplevelDragManager::handleViewUnmap(wl_listener* listener, void* /*data*/) {
    Drag* drag = wl_container_of(listener, drag, viewUnmap);
    detach(*drag);
  }

  void ToplevelDragManager::handleViewDestroy(wl_listener* listener, void* /*data*/) {
    Drag* drag = wl_container_of(listener, drag, viewDestroy);
    detach(*drag);
  }

  void ToplevelDragManager::detach(Drag& drag) {
    removeListener(drag.viewMap);
    removeListener(drag.viewUnmap);
    removeListener(drag.viewDestroy);
    drag.view = nullptr;
  }

  void ToplevelDragManager::dragStarted(wlr_drag* drag) {
    const auto found = std::ranges::find_if(m_drags, [drag](const auto& entry) {
      return entry->source != nullptr && entry->source == drag->source;
    });
    if (found == m_drags.end()) {
      return;
    }
    Drag& entry = **found;
    entry.drag = drag;
    entry.dragDestroy.notify = handleDragDestroy;
    wl_signal_add(&drag->events.destroy, &entry.dragDestroy);
    if (entry.view != nullptr && entry.view->mapped()) {
      const wlr_cursor* cursor = m_server.cursor()->wlr();
      follow(entry, cursor->x, cursor->y);
    }
  }

  ToplevelDragManager::Drag* ToplevelDragManager::running() const {
    const auto found = std::ranges::find_if(m_drags, [](const auto& drag) {
      return drag->drag != nullptr && drag->view != nullptr && drag->view->mapped();
    });
    return found != m_drags.end() ? found->get() : nullptr;
  }

  View* ToplevelDragManager::draggedView() const {
    const Drag* drag = running();
    return drag != nullptr ? drag->view : nullptr;
  }

  void ToplevelDragManager::pointerMoved(double x, double y) {
    if (Drag* drag = running()) {
      follow(*drag, x, y);
    }
  }

  // The window floats with the pointer the way an interactive move carries it, its geometry origin at the offset.
  void ToplevelDragManager::follow(Drag& drag, double x, double y) {
    View* view = drag.view;
    if (view->tiled()) {
      view->setFloating(true, false);
    }
    view->setPosition(static_cast<int>(std::lround(x)) - drag.offsetX, static_cast<int>(std::lround(y)) - drag.offsetY);
  }

  // As an interactive move ends: the window joins the workspace under the pointer and keeps its place.
  void ToplevelDragManager::dropped(Drag& drag) {
    View* view = drag.view;
    if (view == nullptr || !view->mapped()) {
      return;
    }
    const wlr_cursor* cursor = m_server.cursor()->wlr();
    Output* output = m_server.outputFromWlr(wlr_output_layout_output_at(m_server.outputLayout(), cursor->x, cursor->y));
    if (output != nullptr && output->workspaceGroup() != nullptr) {
      if (Workspace* target = output->workspaceGroup()->active(); target != nullptr && view->workspace() != target) {
        const int x = view->sceneTree()->node.x;
        const int y = view->sceneTree()->node.y;
        view->moveToWorkspace(target);
        view->setPosition(x, y);
      }
    }
    view->rememberFloatingPosition();
  }

  const struct xdg_toplevel_drag_manager_v1_interface ToplevelDragManager::kManagerImplementation = {
      .destroy = ToplevelDragManager::handleManagerDestroy,
      .get_xdg_toplevel_drag = ToplevelDragManager::handleGetDrag,
  };

  const struct xdg_toplevel_drag_v1_interface ToplevelDragManager::kDragImplementation = {
      .destroy = ToplevelDragManager::handleDragDestroyRequest,
      .attach = ToplevelDragManager::handleAttach,
  };

} // namespace umbriel
