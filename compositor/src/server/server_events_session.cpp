#include "config/change.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "config/store.h"
#include "core/log.h"
#include "desktop-unstable-v1-protocol.h"
#include "input/cursor.h"
#include "input/eis.h"
#include "input/gestures.h"
#include "input/input_capture.h"
#include "input/keyboard.h"
#include "input/keyboard_leds.h"
#include "input/seat.h"
#include "input/system_keymap.h"
#include "input/text_input.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/display_store.h"
#include "output/identity.h"
#include "output/mirror.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/allocator.h"
#include "server/backend_manager.h"
#include "server/desktop_output_manager.h"
#include "server/desktop_settings_manager.h"
#include "server/desktop_shell.h"
#include "server/ipc.h"
#include "server/server.h"
#include "server/server_events_internal.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {
    constexpr Logger kLog("server");
    // shell/src/shell/switcher/window_switcher.cpp creates it on open and destroys it on close.
    constexpr std::string_view kWindowSwitcherNamespace = "noctalia-window-switcher";
  } // namespace

  void Server::onNewLayerSurface(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newLayerSurface);
    auto surface = std::make_unique<LayerSurface>(*self, static_cast<wlr_layer_surface_v1*>(data));
    if (surface->layerSurface() == nullptr) {
      return;
    }
    auto* wlrLayer = static_cast<wlr_layer_surface_v1*>(data);
    if (wlrLayer->namespace_ != nullptr && std::string_view(wlrLayer->namespace_) == kWindowSwitcherNamespace) {
      // The shell's app switcher lives exactly as long as this surface: frozen apps thaw while it is open.
      struct ThawWatch {
        Server* server = nullptr;
        wl_listener destroy{};
      };
      auto* watch = new ThawWatch{.server = self, .destroy = {}};
      watch->destroy.notify = [](wl_listener* listener, void* /*data*/) {
        ThawWatch* owner;
        owner = wl_container_of(listener, owner, destroy);
        wl_list_remove(&owner->destroy.link);
        owner->server->holdQosThawIntent(false);
        delete owner;
      };
      wl_signal_add(&wlrLayer->events.destroy, &watch->destroy);
      self->holdQosThawIntent(true);
    }
    self->m_layerSurfaces.push_back(std::move(surface));
    self->scheduleIpcLayersEvent();
  }

  void Server::onNewSessionLock(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newSessionLock);
    self->beginSessionLock(static_cast<wlr_session_lock_v1*>(data));
  }

  void Server::beginSessionLock(wlr_session_lock_v1* lock) {
    if (m_inputCapture != nullptr) {
      m_inputCapture->suspend();
    }
    if (m_pendingCaptureId != 0) {
      dismissConfirmation();
    }
    if (m_sessionLock != nullptr) {
      kLog.info("denying session lock; one is already active");
      wlr_session_lock_v1_destroy(lock);
      return;
    }

    m_sessionLock = std::make_unique<SessionLock>(*this, lock);
    m_sessionLock->start();
  }

  void Server::activateSessionLock(SessionLock* lock) {
    if (m_sessionLock.get() != lock) {
      return;
    }
    if (m_restartLockEscape != nullptr) {
      wl_event_source_remove(m_restartLockEscape);
      m_restartLockEscape = nullptr;
    }
    if (m_lockFileManaged) {
      const std::string path = lockFilePath();
      mkdir(path.substr(0, path.rfind('/')).c_str(), 0700);
      if (const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600); fd >= 0) {
        close(fd);
      } else {
        kLog.error("cannot write the lock file {}: {}", path, std::strerror(errno));
      }
    }

    if (!m_sessionLocked) {
      m_lockFocusOutput.clear();
      if (View* focused = View::fromSurface(m_seat->wlr()->keyboard_state.focused_surface)) {
        if (Workspace* workspace = focused->workspace(); workspace != nullptr && workspace->group() != nullptr) {
          if (const Output* output = workspace->group()->output(); output != nullptr) {
            m_lockFocusOutput = output->wlr()->name;
          }
        }
      }

      dismissConfirmation();
      if (m_inputCapture != nullptr) {
        m_inputCapture->suspend();
      }
      if (m_eisInput != nullptr) {
        m_eisInput->setSessionLocked(true);
      }
      if (m_desktopShell != nullptr) {
        m_desktopShell->releaseKeys();
      }
      m_sessionLocked = true;
      m_effects.setSuspended(true);
      m_effects.applyOutputEffects();
      cancelModifierTap();
      m_overview->forceClose();
      if (m_quitConfirm != nullptr) {
        m_quitConfirm->hide();
      }
      m_cursor->resetMode();
      m_cursor->clearConstraint();
      clearNormalFocus();
      updateIdleInhibit();
      syncSuspendedViews();
    }

    updateLockBlank();
    setLockBlankEnabled(true);
    raiseLockTree();
    if (m_desktopShell != nullptr) {
      m_desktopShell->sendRecovered();
    }
  }

  void Server::unlockSession() {
    m_sessionLocked = false;
    if (m_lockFileManaged) {
      unlink(lockFilePath().c_str());
    }
    if (m_eisInput != nullptr) {
      m_eisInput->setSessionLocked(false);
    }
    m_effects.setSuspended(false);
    m_effects.applyOutputEffects();
    for (const auto& output : m_outputs) {
      if (output->effectEligible() > 0) {
        output->scheduleEffectFrame();
      }
    }
    updateIdleInhibit();
    syncSuspendedViews();
    setLockBlankEnabled(false);
    // The cursor need not sit on the output that had focus, so restore the
    // remembered one. refocus() then keeps that output's active workspace, which
    // is what makes unlocking on an empty workspace stay there.
    Output* output = m_lockFocusOutput.empty() ? nullptr : outputFromName(m_lockFocusOutput);
    m_lockFocusOutput.clear();
    refocus(output);
  }

  void Server::removeSessionLock(SessionLock* lock) {
    if (m_sessionLock.get() != lock) {
      return;
    }
    m_sessionLock.reset();
    if (!m_sessionLocked) {
      wlr_scene_node_set_enabled(&m_lockTree->node, false);
    } else if (m_desktopShell != nullptr) {
      m_desktopShell->sendRelock();
    }
  }

  void Server::setLockBlankEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_lockTree->node, enabled);
    wlr_scene_node_set_enabled(&m_lockBlank->node, enabled);
    if (enabled) {
      raiseLockTree();
    }
  }

  // Security boundary: never defer this through Dirty. One stale frame can
  // expose content that the lock exists to hide.
  void Server::updateLockBlank() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    auto color = config().colors.backdrop;
    color[3] = 1.0F;
    wlr_scene_rect_set_color(m_lockBlank, color.data());
    wlr_scene_rect_set_size(m_lockBlank, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_lockBlank->node, layoutBox.x, layoutBox.y);
  }

  void Server::updateBackdrop() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_set_background_color(m_scene, config().colors.backdrop.data());
    wlr_scene_rect_set_color(m_backdrop, config().colors.backdrop.data());
    wlr_scene_rect_set_size(m_backdrop, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_backdrop->node, layoutBox.x, layoutBox.y);
    for (const auto& output : m_outputs) {
      output->markBlurBackgroundDirty();
    }
  }

  void Server::raiseLockTree() { wlr_scene_node_raise_to_top(&m_lockTree->node); }

  void Server::onLayerKeyboardFocusChange(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_layerKeyboardFocusChange);
    self->scheduleIpcLayersEvent();
  }

  void Server::closeLayerSurfaces(wlr_output* output) {
    std::vector<wlr_layer_surface_v1*> toClose;
    for (const auto& ls : m_layerSurfaces) {
      if (ls->layerSurface() != nullptr && ls->layerSurface()->output == output) {
        toClose.push_back(ls->layerSurface());
      }
    }
    for (wlr_layer_surface_v1* ls : toClose) {
      wlr_layer_surface_v1_destroy(ls);
    }
  }

  void Server::removeLayerSurface(LayerSurface* layerSurface, wlr_output* output) {
    std::erase_if(m_layerSurfaces, [layerSurface](const std::unique_ptr<LayerSurface>& entry) {
      return entry.get() == layerSurface;
    });
    arrangeLayers(output);
    scheduleIpcLayersEvent();
  }

} // namespace andromeda
