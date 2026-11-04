#include "view/popup.h"

#include "layer/layer_surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace andromeda {

  namespace {
    std::vector<Popup*>& livePopups() {
      static std::vector<Popup*> popups;
      return popups;
    }
  } // namespace

  void Popup::applyGlassToAll() {
    for (Popup* popup : livePopups()) {
      popup->m_blur.setMaterial(configGlass(glass::Kind::Menu));
    }
  }

  Popup::Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree, wlr_scene_tree* captureTree) : m_popup(popup) {
    if (parentTree == nullptr) {
      wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(m_popup->parent);
      assert(parent != nullptr);
      parentTree = static_cast<wlr_scene_tree*>(parent->data);
    }
    m_popup->base->data = wlr_scene_xdg_surface_create(parentTree, m_popup->base);
    if (captureTree != nullptr) {
      wlr_scene_xdg_surface_create(captureTree, m_popup->base);
    }

    m_commit.notify = onCommit;
    wl_signal_add(&m_popup->base->surface->events.commit, &m_commit);
    m_reposition.notify = onReposition;
    wl_signal_add(&m_popup->events.reposition, &m_reposition);
    m_map.notify = onMap;
    wl_signal_add(&m_popup->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_popup->base->surface->events.unmap, &m_unmap);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_popup->events.destroy, &m_destroy);
    livePopups().push_back(this);
  }

  Popup::~Popup() {
    std::erase(livePopups(), this);
    if (m_commit.link.next != nullptr) {
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_reposition.link);
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Popup::onCommit(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_commit); // NOLINT(modernize-use-auto)
    self->handleCommit();
  }

  void Popup::onReposition(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_reposition); // NOLINT(modernize-use-auto)
    self->unconstrain();
  }

  void Popup::onMap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_map); // NOLINT(modernize-use-auto)
    self->handleMap();
  }

  void Popup::onUnmap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_unmap); // NOLINT(modernize-use-auto)
    self->m_blur.hide();
  }

  void Popup::onDestroy(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_destroy); // NOLINT(modernize-use-auto)
    self->handleDestroy();
  }

  void Popup::handleCommit() {
    const wlr_box& geometry = m_popup->base->geometry;
    SurfaceBlurOptions blurOptions;
    const pixman_region32_t* region = nullptr;
    wlr_surface* surface = m_popup->base->surface;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      blurOptions = layer->popupBlurOptions();
      region = layer->clientBlurRegion(surface);
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      blurOptions = view->popupBlurOptions();
      region = view->clientBlurRegion(surface);
    }
    // A client blur region turns popup blur on, as it does for windows; it is the exact shape, so no alpha test.
    if (region != nullptr && pixman_region32_not_empty(region)) {
      blurOptions.enabled = true;
      blurOptions.ignoreAlpha = 0.0F;
    } else {
      region = nullptr;
    }
    m_blur.setMaterial(configGlass(glass::Kind::Menu));
    m_blur.update(
        static_cast<wlr_scene_tree*>(m_popup->base->data), surface, wlr_box{0, 0, geometry.width, geometry.height},
        geometry, corner_radii_none(), nullptr, blurOptions, 1.0F, nullptr, region
    );
    if (!m_popup->base->initial_commit) {
      return;
    }

    // New popups miss the parent's map/workspace-time scale notify; tell them the parent's output scale before the
    // first configure so scale-aware clients size and map input correctly from frame one.
    Output* output = nullptr;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      output = layer->output();
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      output = view->currentOutput();
    }
    if (output != nullptr) {
      wlr_xdg_surface_for_each_surface(m_popup->base, &Output::notifySurfaceScaleIter, output);
    }

    unconstrain();
  }

  void Popup::unconstrain() {
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->unconstrainPopup(m_popup);
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      view->unconstrainPopup(m_popup);
    } else {
      wlr_xdg_surface_schedule_configure(m_popup->base);
    }
  }

  void Popup::handleMap() {
    // Only a grabbing popup owns the keyboard. A toplevel's popup reaches it through its already focused window.
    if (m_popup->seat == nullptr) {
      return;
    }
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->focusForPopupGrab();
    }
  }

  void Popup::handleDestroy() {
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_reposition.link);
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_destroy.link);
    m_commit.link.next = nullptr;
    m_reposition.link.next = nullptr;
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_destroy.link.next = nullptr;
    delete this;
  }

} // namespace andromeda
