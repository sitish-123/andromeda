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
#include "scene/cheatsheet.h"
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
    using server_events::viewForToplevel;
    using server_events::applyPointerConfig;
  } // namespace

  void Server::applyConfig(const ConfigEffects& effects) {
    if (!effects.any()) {
      return;
    }
    // Settle every interaction whose meaning depends on the workspace axis or the
    // layout objects about to be replaced, while those still exist.
    if (effects.workspaceLayout || effects.workspaceInventory || effects.outputState) {
      m_cursor->cancelLayoutInteraction();
      m_gestures->cancelForLayoutChange();
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->slideFinish();
        }
      }
    }
    if (effects.effects) {
      reconcileEffectSelections();
    }
    if (effects.animation || effects.effects) {
      effectRegistry().prepare(m_renderer);
    }
    if (effects.effects) {
      // The next frame re-arms the effect timer from the new max_fps.
      for (const auto& output : m_outputs) {
        if (output->effectEligible() > 0) {
          output->scheduleEffectFrame();
        }
      }
    }

    if (effects.sceneBlur) {
      const Config::Appearance::Blur& blur = config().appearance.blur;
      wlr_scene_set_blur_data(
          m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
          static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
      );
      for (const auto& output : m_outputs) {
        output->markDirty(Dirty::LayerArrange);
      }
    }
    if (effects.input) {
      applyKeyboardConfig();
      for (const auto& pointer : m_pointers) {
        applyPointerConfig(pointer->device);
      }
      for (const auto& tablet : m_tabletDevices) {
        applyTabletConfig(*tablet);
      }
      for (const auto& touch : m_touchDevices) {
        applyTouchConfig(*touch);
      }
      for (const auto& pad : m_tabletPads) {
        applyTabletPadConfig(*pad);
      }
      m_seat->applyConfig();
      m_cursor->applyConfig();
      for (const auto& output : m_outputs) {
        output->applyCursorConfig();
      }
    }
    if (effects.internalUi) {
      markDirty(Dirty::Cheatsheet);
    }
    if (effects.outputState) {
      // A successful output-policy reload is the explicit boundary for
      // temporary output-management choices, including overrides retained for
      // a currently disconnected monitor.
      m_outputEnableOverrides.clear();
      applyConfiguredMirrors();
      m_deferOutputManagerConfig = true;
      for (const auto& output : m_outputs) {
        (void)output->applyOutputState();
      }
      m_deferOutputManagerConfig = false;
      for (const auto& output : m_outputs) {
        if (output->desktopEnabled()) {
          continue;
        }
        Output* fallback = nullptr;
        for (const auto& candidate : m_outputs) {
          if (candidate.get() != output.get() && candidate->desktopEnabled() && candidate->wlr()->enabled) {
            fallback = candidate.get();
            break;
          }
        }
        reassignOutputViews(output.get(), fallback);
      }
      scheduleDisplacedViewRestore();
      updateOutputManagerConfig();
      // A disabled output must not keep keyboard focus: pull it onto a live one.
      refocus();
      // Scale may have changed; every surface must hear about it or scale-aware
      // clients keep mapping input with the stale scale.
      refreshSurfaceScales();
    }
    if (effects.tearingPolicy) {
      for (const auto& output : m_outputs) {
        output->resetTearingState();
      }
    }
    if (effects.directScanoutPolicy) {
      for (const auto& output : m_outputs) {
        output->applyDirectScanoutConfig();
      }
    }
    if (effects.workspaceInventory) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->reconcileInventory();
        }
      }
    }
    if (effects.workspaceLayout) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->refreshLayouts();
        }
      }
      m_cursor->cancelStaleTiledResize();
    }
    if (effects.viewChrome) {
      for (View* view : sortedEffectViews()) {
        view->refreshConfigChrome();
      }
      // Screen and cursor presets take their palette from [colors] too.
      m_effects.applyOutputEffects();
      markDirty(Dirty::Backdrop);
      if (m_sessionLocked) {
        updateLockBlank();
      }
    }
    if (effects.effects && !effects.viewChrome) {
      for (View* view : sortedEffectViews()) {
        view->applyDynamicRules();
      }
    }
    if (effects.animation && m_scratchpadManager != nullptr) {
      m_scratchpadManager->applyConfig();
    }
    // A maximized scratchpad window follows the gap, border, and struts it is sized with; no workspace arrange reaches
    // it.
    if ((effects.workspaceLayout || effects.viewChrome) && m_scratchpadManager != nullptr) {
      m_scratchpadManager->refitMaximized();
    }
    if (effects.layerEffects) {
      for (const auto& layer : m_layerSurfaces) {
        if (layer->mapped()) {
          layer->applyConfig();
        }
      }
    }
    if (effects.dialogAttachment) {
      // Focus moves promote views in the registry, so walk a snapshot.
      std::vector<View*> views;
      views.reserve(m_registry.all().size());
      for (const auto& view : m_registry.all()) {
        views.push_back(view.get());
      }
      for (View* view : views) {
        if (view->mapped()) {
          view->reconcileAttachment();
        }
      }
    }
  }

  void Server::applyKeyboardConfig() {
    m_surfaceLayouts.clear();
    for (const auto& keyboard : m_keyboards) {
      keyboard->applyConfig();
      if (m_inputCapture != nullptr && !keyboard->virtualDevice()) {
        m_inputCapture->updateKeymap(keyboard->wlr());
      }
      // A rebuilt keymap can gain locks absent from its predecessor. Reapply
      // shared state after the per-device restoration has finished.
      refreshKeyboardLeds();
    }
    if (m_eisInput != nullptr) {
      m_eisInput->applyConfig();
    }
    if (m_keyboardLayoutSource != nullptr) {
      syncKeyboardLayout(m_keyboardLayoutSource);
      notifyKeyboardLayoutIpc();
    }
  }

  bool Server::refreshSystemKeymap() {
    XkbNames names = usesSystemXkbNames(config().input) ? querySystemKeymap() : XkbNames{};
    if (names == m_systemKeymap) {
      return false;
    }
    if (!names.empty()) {
      kLog.info(
          "using the systemd-localed keymap: layout='{}' variant='{}' options='{}' model='{}'", names.layout,
          names.variant, names.options, names.model
      );
    }
    m_systemKeymap = std::move(names);
    return true;
  }

  void Server::handleConfigReload(const ConfigReloadResult& result) {
    cancelModifierTap();
    if (result.success) {
      const bool systemKeymapChanged = refreshSystemKeymap();
      if (result.change.drm) {
        kLog.warn("DRM configuration changed; restart Andromeda to apply it");
      }
      if (result.change.keybinds) {
        m_bindCooldowns.clear();
      }
      if (result.change.scratchpads && m_scratchpadManager != nullptr) {
        m_scratchpadManager->reconcileConfig();
      }
      if (result.effects.invalidatesOverview()) {
        // A four-finger gesture must not reopen a presentation this close invalidates.
        m_gestures->cancelForLayoutChange();
        m_overview->forceClose();
      }
      applyConfig(result.effects);
      if (m_desktopSettingsManager != nullptr) {
        m_desktopSettingsManager->settingsChanged();
      }
      if (m_desktopOutputManager != nullptr) {
        m_desktopOutputManager->propertiesChanged();
      }
      if (systemKeymapChanged && !result.effects.input) {
        applyKeyboardConfig();
      }
      if ((result.change.colors || result.change.appearance) && m_ipc != nullptr) {
        m_ipc->notifyThemeChanged();
      }
      const std::string changed = result.change.summary();
      const std::string effects = result.effects.summary();
      kLog.info(
          "config reloaded (sections: {}; effects: {})", changed.empty() ? "none" : changed,
          effects.empty() ? "none" : effects
      );
    }
    showConfigDiagnostics();
    markDirty(Dirty::Cheatsheet);
    if (m_configWatcher != nullptr) {
      m_configWatcher->watch(configWatchPaths());
    }
  }

  // Sends frame callbacks to toplevels off the rendered scene that could not be suspended (see
  // kBackgroundFrameIntervalMs).
  int Server::onBackgroundFrameTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (const auto& view : self->m_registry.all()) {
      // A presented view gets its callbacks from the outputs that draw it, including the outgoing workspace mid-slide,
      // and a background_render view from its own faster tick. A suspended one gets them only to commit a configure it
      // still owes, such as a resize while hidden.
      if (!view->mapped()
          || view->seen()
          || view->backgroundRender()
          || (view->suspended() && !view->configureOutstanding())) {
        continue;
      }
      view->forEachSurface(
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }

    if (self->m_backgroundFrameTimer != nullptr) {
      wl_event_source_timer_update(self->m_backgroundFrameTimer, kBackgroundFrameIntervalMs);
    }
    return 0;
  }

  void Server::syncSuspendedViews() {
    for (const auto& view : m_registry.all()) {
      view->syncSuspendedState();
    }
    armBackgroundRenderTimer();
  }

  namespace {
    // A hidden background_render window's callback interval: its output's refresh period, or 60 Hz when the output
    // reports none (headless and nested outputs).
    int backgroundRenderIntervalMs(const View& view) {
      const Output* output = view.currentOutput();
      const int refreshMhz = output != nullptr ? output->wlr()->refresh : 0;
      if (refreshMhz <= 0) {
        return 16;
      }
      return std::max(1, (1'000'000 + refreshMhz / 2) / refreshMhz);
    }

    bool needsBackgroundRender(const View& view) {
      return view.mapped() && view.backgroundRender() && !view.seen();
    }
  } // namespace

  int Server::onBackgroundRenderTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (const auto& view : self->m_registry.all()) {
      if (!needsBackgroundRender(*view)) {
        continue;
      }
      view->forEachSurface(
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }
    self->armBackgroundRenderTimer();
    return 0;
  }

  void Server::armBackgroundRenderTimer() {
    if (m_backgroundRenderTimer == nullptr) {
      return;
    }
    int interval = 0;
    for (const auto& view : m_registry.all()) {
      if (needsBackgroundRender(*view)) {
        const int candidate = backgroundRenderIntervalMs(*view);
        interval = interval == 0 ? candidate : std::min(interval, candidate);
      }
    }
    // Zero disarms the timer.
    wl_event_source_timer_update(m_backgroundRenderTimer, interval);
  }

  void Server::scheduleSuspensionSync() {
    if (m_stopping || m_suspensionSyncIdle != nullptr) {
      return;
    }
    m_suspensionSyncIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onSuspensionSyncIdle, this);
    if (m_suspensionSyncIdle == nullptr) {
      kLog.error("failed to register suspension sync idle source");
    }
  }

  void Server::onSuspensionSyncIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_suspensionSyncIdle = nullptr;
    for (const auto& view : server->m_registry.all()) {
      view->syncSuspendedState();
    }
    server->armBackgroundRenderTimer();
  }

  bool Server::viewRenderedOffscreen(const View& view) const {
    if (view.backgroundRender() || (m_overview != nullptr && m_overview->presentsLive(&view))) {
      return true;
    }
    return viewCaptured(view);
  }

  bool Server::viewCaptured(const View& view) const {
    return view.m_captureSource != nullptr
        && std::ranges::any_of(m_imageCopySessions, [&view](const std::unique_ptr<ImageCopySessionWatch>& watch) {
             return watch->session != nullptr && watch->session->source == view.m_captureSource;
           });
  }

  int Server::onStartupRulesTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    for (const auto& view : self->m_registry.all()) {
      view->refreshStartupRuleEffects();
    }
    for (const auto& layer : self->m_layerSurfaces) {
      layer->refreshStartupRuleEffects();
    }
    return 0;
  }

  // Fires when the underlying GL context is invalidated (GPU reset, VRAM lost after suspend, driver-detected hang); the
  // renderer must be rebuilt, since GL calls into a dead context do nothing. Defer rebuilding until this signal and the
  // failed render call have both unwound.
  void Server::onRendererLost(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_rendererLost);
    if (self->m_stopping || self->m_rendererRecoveryIdle != nullptr) {
      return;
    }
    self->m_rendererRecoveryIdle =
        wl_event_loop_add_idle(wl_display_get_event_loop(self->m_display), onRendererRecoveryIdle, self);
    if (self->m_rendererRecoveryIdle == nullptr) {
      kLog.error("could not defer renderer recovery, terminating");
      self->stop();
    }
  }

  void Server::onRendererRecoveryIdle(void* data) {
    auto* self = static_cast<Server*>(data);
    self->m_rendererRecoveryIdle = nullptr;
    if (!self->m_stopping) {
      self->recreateRenderer();
    }
  }

  void Server::recreateRenderer() {
    kLog.warn("GPU context lost, recreating renderer");

    wlr_renderer* oldRenderer = m_renderer;
    wlr_allocator* oldAllocator = m_allocator;

    wlr_renderer* newRenderer = m_backendManager->createRenderer();
    if (newRenderer == nullptr) {
      kLog.error("could not recreate fx_renderer after GPU reset, terminating");
      stop();
      return;
    }
    wlr_allocator* newAllocator = createAllocator(m_backend, newRenderer);
    if (newAllocator == nullptr) {
      kLog.error("could not recreate allocator after GPU reset, terminating");
      wlr_renderer_destroy(newRenderer);
      stop();
      return;
    }
    if (!m_backendManager->verifyOpenDevices("after renderer recovery")) {
      wlr_allocator_destroy(newAllocator);
      wlr_renderer_destroy(newRenderer);
      kLog.error("renderer recovery opened an excluded GPU, terminating");
      stop();
      return;
    }

    // Rewire the lost signal onto the new renderer BEFORE swapping the pointers so that
    // a second reset during recreation is delivered.
    wl_list_remove(&m_rendererLost.link);
    wl_signal_add(&newRenderer->events.lost, &m_rendererLost);

    for (const auto& output : m_outputs) {
      if (auto* group = output->workspaceGroup()) {
        group->cancelReveal();
      }
    }
    m_renderer = newRenderer;
    m_allocator = newAllocator;
    effectRegistry().prepare(m_renderer);
    // Rebind existing caches before the old renderer invalidates its programs.
    // A frozen clock may deduplicate every subsequent animation tick.
    for (const auto& view : m_registry.all()) {
      if (view->mapped()) {
        view->syncAnimationEffects();
      }
    }
    if (m_overview != nullptr) {
      m_overview->refreshEffectBindings();
    }

    // Point the compositor at the new renderer so clients' shm/dma-buf textures get
    // re-imported on next attach.
    wlr_compositor_set_renderer(m_compositor, newRenderer);

    // Re-init every output's render pipeline with the new renderer/allocator, and force
    // a fresh frame so damage tracking rebuilds from scratch.
    for (const auto& output : m_outputs) {
      wlr_output* wlrOutput = output->wlr();
      wlr_output_init_render(wlrOutput, newAllocator, newRenderer);
      wlr_output_schedule_frame(wlrOutput);
    }

    wlr_allocator_destroy(oldAllocator);
    wlr_renderer_destroy(oldRenderer);

    kLog.info("renderer recreated");
  }

  void
  Server::onProtocolMessage(void* data, wl_protocol_logger_type direction, const wl_protocol_logger_message* message) {
    if (direction != WL_PROTOCOL_LOGGER_REQUEST
        || message == nullptr
        || message->resource == nullptr
        || message->message == nullptr
        || message->arguments_count != 1
        || message->arguments == nullptr) {
      return;
    }
    const char* resourceClass = wl_resource_get_class(message->resource);
    if (resourceClass == nullptr
        || std::string_view(resourceClass) != "xdg_toplevel"
        || std::string_view(message->message->name) != "set_parent") {
      return;
    }

    auto* server = static_cast<Server*>(data);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(message->resource);
    if (View* view = viewForToplevel(*server, toplevel)) {
      // wlroots has not handled the request yet, so the raw nullable object is
      // still available even when its toplevel has not mapped.
      view->recordOpeningParentRequest(message->arguments[0].o != nullptr);
    }
  }

  void Server::onNewIdleInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newIdleInhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    auto* watch = new IdleInhibitorWatch();
    watch->server = self;
    watch->destroy.notify = onIdleInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->updateIdleInhibit();
  }

  void Server::onIdleInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    IdleInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    delete watch;
    server->updateIdleInhibit();
  }

  void Server::onNewImageCopySession(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newImageCopySession);
    auto* session = static_cast<wlr_ext_image_copy_capture_session_v1*>(data);
    auto watch = std::make_unique<ImageCopySessionWatch>();
    watch->server = self;
    watch->session = session;
    watch->output = wlr_output_try_from_ext_image_capture_source_v1(session->source);
    watch->destroy.notify = onImageCopySessionDestroy;
    wl_signal_add(&session->events.destroy, &watch->destroy);
    self->m_imageCopySessions.push_back(std::move(watch));
    self->scheduleSuspensionSync();
    self->refreshCaptureRuleEffects(session->source);
  }

  // The session's render lock is released after this signal; the frame it schedules runs from an idle, without it.
  void Server::onImageCopySessionDestroy(wl_listener* listener, void* /*data*/) {
    ImageCopySessionWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wlr_ext_image_capture_source_v1* source = watch->session->source;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_imageCopySessions, [watch](const std::unique_ptr<ImageCopySessionWatch>& entry) {
      return entry.get() == watch;
    });
    for (const auto& output : server->m_outputs) {
      output->scheduleEffectCaptureRelease();
    }
    server->scheduleSuspensionSync();
    server->refreshCaptureRuleEffects(source);
  }

  // `match.is_cast_target` follows the sessions on a window's source. Output sources match no view and change nothing.
  // Closing a cast window ends its session from inside ~View, while the registry holds the dying entry as null.
  void Server::refreshCaptureRuleEffects(const wlr_ext_image_capture_source_v1* source) {
    for (const auto& view : m_registry.all()) {
      if (view != nullptr && view->m_captureSource == source) {
        view->refreshStateRuleEffects();
      }
    }
  }

  void Server::onNewShortcutsInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newShortcutsInhibitor);
    auto* inhibitor = static_cast<wlr_keyboard_shortcuts_inhibitor_v1*>(data);
    auto watch = std::make_unique<ShortcutsInhibitorWatch>();
    watch->server = self;
    watch->inhibitor = inhibitor;
    watch->destroy.notify = onShortcutsInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->m_shortcutsInhibitors.push_back(std::move(watch));
    self->refreshKeyboardShortcutsInhibitors();
    kLog.debug("keyboard shortcuts inhibitor {}", inhibitor->active ? "activated" : "blocked by window rule");
  }

  void Server::onShortcutsInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    ShortcutsInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_shortcutsInhibitors, [watch](const std::unique_ptr<ShortcutsInhibitorWatch>& candidate) {
      return candidate.get() == watch;
    });
    kLog.debug("keyboard shortcuts inhibitor removed");
  }
  void Server::onPointerDestroy(wl_listener* listener, void* /*data*/) {
    PointerDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    if (server->m_inputCapture != nullptr) {
      server->m_inputCapture->forgetAbsoluteDevice(watch->device);
      if ((server->m_inputCapture->capabilities() & 2U) != 0) {
        server->m_inputCapture->suspend();
      }
    }
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_pointers, [watch](const std::unique_ptr<PointerDevice>& pointer) {
      return pointer.get() == watch;
    });
    server->inputDevicesChanged();
  }

  void Server::onWorkspaceCommit(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_workspaceCommit);
    self->handleWorkspaceCommit(data);
  }

  void Server::handleWorkspaceCommit(void* data) {
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link) {
      switch (request->type) {
      case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE: {
        WorkspaceGroup* group = workspaceGroupFromHandle(request->create_workspace.group);
        if (group != nullptr) {
          group->createWorkspace(request->create_workspace.name);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->activate.workspace)) {
          workspace->group()->activate(workspace);
          refocus(workspace->group()->output());
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->deactivate.workspace)) {
          workspace->group()->deactivate(workspace);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
        // Workspaces stay bound to their output group.
        break;
      case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
        break;
      }
    }
  }

  Workspace* Server::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<Workspace*>(handle->data);
  }

  WorkspaceGroup* Server::workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<WorkspaceGroup*>(handle->data);
  }

  void Server::updateIdleInhibit() {
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor;
    wl_list_for_each(inhibitor, &m_idleInhibitManager->inhibitors, link) {
      wlr_surface* root = wlr_surface_get_root_surface(inhibitor->surface);
      if (root == nullptr) {
        continue;
      }

      if (m_sessionLocked) {
        wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(root);
        inhibited =
            lockSurface != nullptr && root->mapped && lockSurface->output != nullptr && lockSurface->output->enabled;
      } else if (View* view = View::fromSurface(root)) {
        int x = 0;
        int y = 0;
        inhibited = view->mapped() && wlr_scene_node_coords(&view->sceneTree()->node, &x, &y);
      } else if (wlr_layer_surface_v1* wlrLayer = wlr_layer_surface_v1_try_from_wlr_surface(root)) {
        auto* layer = static_cast<LayerSurface*>(wlrLayer->data);
        Output* output = layer != nullptr ? layer->output() : nullptr;
        int x = 0;
        int y = 0;
        inhibited = layer != nullptr
            && layer->mapped()
            && output != nullptr
            && output->wlr()->enabled
            && wlr_scene_node_coords(&layer->scene()->tree->node, &x, &y);
      }

      if (inhibited) {
        break;
      }
    }
    if (inhibited != m_idleInhibited) {
      m_idleInhibited = inhibited;
      kLog.debug("idle {}", inhibited ? "inhibited" : "no longer inhibited");
    }
    wlr_idle_notifier_v1_set_inhibited(m_idleNotifier, inhibited);
  }

  void Server::notifyIdleActivity() { wlr_idle_notifier_v1_notify_activity(m_idleNotifier, m_seat->wlr()); }

  void Server::notifyInputActivity() {
    wakeDpmsOutputs();
    notifyIdleActivity();
  }

  void Server::wakeDpmsOutputs() {
    const bool anyPowered = std::ranges::any_of(m_outputs, [](const std::unique_ptr<Output>& output) {
      return output->desktopEnabled() && !output->dpmsOff();
    });
    if (anyPowered) {
      return;
    }
    for (const auto& output : m_outputs) {
      if (output->dpmsOff()) {
        (void)output->setPowered(true);
      }
    }
  }

  void Server::notifyOverviewChanged() {
    if (m_ipc != nullptr) {
      m_ipc->notifyOverviewChanged();
    }
  }

  void Server::notifySubmapChanged() {
    if (m_ipc != nullptr) {
      m_ipc->notifySubmapChanged();
    }
  }

  void Server::scheduleIpcWindowsEvent() {
    if (m_ipc == nullptr || m_ipcWindowsIdle != nullptr) {
      return;
    }
    m_ipcWindowsIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onIpcWindowsIdle, this);
    if (m_ipcWindowsIdle == nullptr) {
      kLog.error("failed to register IPC windows idle source");
    }
  }

  void Server::onIpcWindowsIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_ipcWindowsIdle = nullptr;
    if (server->m_ipc != nullptr) {
      server->m_ipc->notifyWindowsChanged();
    }
  }

  void Server::scheduleIpcWorkspacesEvent() {
    // Outputs report their active workspace and focus, which move with the workspace list.
    scheduleIpcOutputsEvent();
    if (m_ipc == nullptr || m_ipcWorkspacesIdle != nullptr) {
      return;
    }
    m_ipcWorkspacesIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onIpcWorkspacesIdle, this);
    if (m_ipcWorkspacesIdle == nullptr) {
      kLog.error("failed to register IPC workspaces idle source");
    }
  }

  void Server::onIpcWorkspacesIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_ipcWorkspacesIdle = nullptr;
    if (server->m_ipc != nullptr) {
      server->m_ipc->notifyWorkspacesChanged();
    }
  }

  void Server::scheduleIpcLayersEvent() {
    if (m_ipc == nullptr || m_ipcLayersIdle != nullptr) {
      return;
    }
    m_ipcLayersIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onIpcLayersIdle, this);
    if (m_ipcLayersIdle == nullptr) {
      kLog.error("failed to register IPC layers idle source");
    }
  }

  void Server::onIpcLayersIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_ipcLayersIdle = nullptr;
    if (server->m_ipc != nullptr) {
      server->m_ipc->notifyLayersChanged();
    }
  }

  void Server::scheduleIpcOutputsEvent() {
    if (m_ipc == nullptr || m_ipcOutputsIdle != nullptr) {
      return;
    }
    m_ipcOutputsIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onIpcOutputsIdle, this);
    if (m_ipcOutputsIdle == nullptr) {
      kLog.error("failed to register IPC outputs idle source");
    }
  }

  void Server::onIpcOutputsIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_ipcOutputsIdle = nullptr;
    if (server->m_ipc != nullptr) {
      server->m_ipc->notifyOutputsChanged();
    }
  }

} // namespace andromeda
