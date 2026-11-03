#include "core/cpu_boost.h"
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

    View* viewForSurface(Server& server, wlr_surface* surface) {
      if (surface == nullptr) {
        return nullptr;
      }
      if (View* view = View::fromSurface(surface)) {
        return view;
      }
      wlr_surface* root = wlr_surface_get_root_surface(surface);
      return viewForToplevel(server, wlr_xdg_toplevel_try_from_wlr_surface(root));
    }

    bool sameWaylandClient(const wlr_surface* first, const wlr_surface* second) {
      if (first == nullptr || second == nullptr || first->resource == nullptr || second->resource == nullptr) {
        return false;
      }
      return wl_resource_get_client(first->resource) == wl_resource_get_client(second->resource);
    }

    std::optional<std::string> processEnvironmentValue(pid_t pid, std::string_view name) {
      if (pid <= 0 || name.empty()) {
        return std::nullopt;
      }
      constexpr size_t kMaximumEnvironmentSize = 1024 * 1024;
      std::ifstream stream("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      if (!stream) {
        return std::nullopt;
      }
      std::string environment(kMaximumEnvironmentSize, '\0');
      stream.read(environment.data(), static_cast<std::streamsize>(environment.size()));
      const auto size = static_cast<size_t>(stream.gcount());
      if (size == environment.size()) {
        return std::nullopt;
      }
      environment.resize(size);
      const std::string prefix = std::string(name) + "=";
      size_t offset = 0;
      while (offset < environment.size()) {
        const size_t end = environment.find('\0', offset);
        const size_t length = (end == std::string::npos ? environment.size() : end) - offset;
        const std::string_view entry(environment.data() + offset, length);
        if (entry.starts_with(prefix)) {
          return std::string(entry.substr(prefix.size()));
        }
        if (end == std::string::npos) {
          break;
        }
        offset = end + 1;
      }
      return std::nullopt;
    }

    wlr_xdg_toplevel_decoration_v1_mode resolvedDecorationMode(wlr_xdg_toplevel_decoration_v1* decoration) {
      // Only clients whose connection prefers SSD can see the manager. Honor
      // an explicit request, otherwise keep the server-side preference.
      wlr_xdg_toplevel_decoration_v1_mode mode = decoration->requested_mode;
      if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
        mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
      }
      return mode;
    }

    struct XdgDecorationWatch {
      wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
      wlr_xdg_toplevel_decoration_v1_mode pendingMode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
      wl_listener requestMode{};
      wl_listener surfaceCommit{};
      wl_listener destroy{};
    };

    void clearDecorationCommit(XdgDecorationWatch* watch) {
      if (watch->surfaceCommit.notify == nullptr) {
        return;
      }
      wl_list_remove(&watch->surfaceCommit.link);
      watch->surfaceCommit.notify = nullptr;
    }

    void onDecorationSurfaceCommit(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, surfaceCommit);
      if (watch->decoration == nullptr
          || watch->decoration->toplevel == nullptr
          || !watch->decoration->toplevel->base->initial_commit) {
        return;
      }
      wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
      clearDecorationCommit(watch);
    }

    void applyXdgDecorationMode(XdgDecorationWatch* watch) {
      if (watch == nullptr || watch->decoration == nullptr || watch->decoration->toplevel == nullptr) {
        return;
      }
      wlr_xdg_surface* surface = watch->decoration->toplevel->base;
      if (surface == nullptr) {
        return;
      }
      watch->pendingMode = resolvedDecorationMode(watch->decoration);
      if (View* view = View::fromSurface(surface->surface))
        view->setXdgDecorationPreference(watch->pendingMode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
      // set_mode schedules a configure; that asserts unless the xdg_surface is ready.
      if (surface->initialized) {
        clearDecorationCommit(watch);
        wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
        return;
      }
      if (watch->surfaceCommit.notify != nullptr) {
        return;
      }
      watch->surfaceCommit.notify = onDecorationSurfaceCommit;
      wl_signal_add(&surface->surface->events.commit, &watch->surfaceCommit);
    }

    void onDecorationRequestMode(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, requestMode);
      applyXdgDecorationMode(watch);
    }

    void onDecorationDestroy(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (watch->decoration != nullptr) {
        if (watch->decoration->toplevel != nullptr && watch->decoration->toplevel->base != nullptr) {
          if (View* view = View::fromSurface(watch->decoration->toplevel->base->surface))
            view->setXdgDecorationPreference(std::nullopt);
        }
        watch->decoration->data = nullptr;
      }
      wl_list_remove(&watch->requestMode.link);
      clearDecorationCommit(watch);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

    struct LegacyDecorationWatch {
      wlr_server_decoration* decoration = nullptr;
      wl_listener mode{};
      wl_listener destroy{};
    };

    void applyLegacyDecorationMode(wlr_server_decoration* decoration) {
      if (View* view = View::fromSurface(decoration->surface))
        view->setLegacyDecorationPreference(decoration->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    }

    void onLegacyDecorationMode(wl_listener* listener, void*) {
      LegacyDecorationWatch* watch;
      watch = wl_container_of(listener, watch, mode);
      applyLegacyDecorationMode(watch->decoration);
    }

    void onLegacyDecorationDestroy(wl_listener* listener, void*) {
      LegacyDecorationWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (View* view = View::fromSurface(watch->decoration->surface))
        view->setLegacyDecorationPreference(std::nullopt);
      wl_list_remove(&watch->mode.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }
  } // namespace

  namespace server_events {
    View* viewForToplevel(Server& server, wlr_xdg_toplevel* toplevel) {
      if (toplevel == nullptr) {
        return nullptr;
      }
      for (const auto& entry : server.registry().all()) {
        if (entry->toplevel() == toplevel) {
          return entry.get();
        }
      }
      return nullptr;
    }
  } // namespace server_events

  void Server::onNewXdgToplevel(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgToplevel);
    View& view = self->adoptView(std::make_unique<View>(*self, static_cast<wlr_xdg_toplevel*>(data)));
    wlr_server_decoration* decoration = nullptr;
    wl_list_for_each(decoration, &self->m_serverDecorationManager->decorations, link) {
      if (decoration->surface == view.rootSurface()) {
        view.setLegacyDecorationPreference(decoration->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
        break;
      }
    }
  }

  View& Server::adoptView(std::unique_ptr<View> view) {
    View& adopted = m_registry.add(std::move(view));
    assignLaunchOriginFromEnvironment(adopted);
    return adopted;
  }

  void Server::onSetXdgToplevelTag(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_setXdgToplevelTag);
    const auto* event = static_cast<wlr_xdg_toplevel_tag_manager_v1_set_tag_event*>(data);
    if (View* view = viewForToplevel(*self, event->toplevel)) {
      view->setXdgTag(event->tag != nullptr ? event->tag : "");
    }
  }

  void Server::onSetXdgToplevelIcon(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_setXdgToplevelIcon);
    const auto* event = static_cast<wlr_xdg_toplevel_icon_manager_v1_set_icon_event*>(data);
    if (View* view = viewForToplevel(*self, event->toplevel)) {
      view->setIconName(event->icon != nullptr && event->icon->name != nullptr ? event->icon->name : "");
    }
  }

  void Server::onSystemBellRing(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_systemBellRing);
    const auto* event = static_cast<wlr_xdg_system_bell_v1_ring_event*>(data);
    const View* view = event->surface != nullptr ? View::fromSurface(event->surface) : nullptr;
    const char* appId = view != nullptr ? view->toplevel()->app_id : nullptr;
    if (self->m_desktopShell == nullptr || !self->m_desktopShell->sendBell(appId != nullptr ? appId : "")) {
      kLog.debug("system bell from '{}' with no shell to ring it", appId != nullptr ? appId : "");
    }
  }

  void Server::onNewXdgDialog(wl_listener* /*listener*/, void* data) {
    auto* dialog = static_cast<wlr_xdg_dialog_v1*>(data);
    if (View* view = View::fromSurface(dialog->xdg_toplevel->base->surface)) {
      view->setDialog(dialog);
    }
  }

  void Server::onNewXdgPopup(wl_listener* /*listener*/, void* data) {
    auto* popup = static_cast<wlr_xdg_popup*>(data);
    // Layer-shell popups (and any popup without a parent yet) are handled
    // elsewhere; parent can be null when xdg_shell emits new_popup.
    if (popup->parent == nullptr || wlr_xdg_surface_try_from_wlr_surface(popup->parent) == nullptr) {
      return;
    }
    View* owner = View::fromSurface(popup->parent);
    new Popup(popup, nullptr, owner != nullptr ? owner->captureTree() : nullptr);
  }

  void Server::onNewXdgDecoration(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgDecoration);
    auto* decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);
    auto* watch = new XdgDecorationWatch{.decoration = decoration};
    decoration->data = watch;
    watch->requestMode.notify = onDecorationRequestMode;
    wl_signal_add(&decoration->events.request_mode, &watch->requestMode);
    watch->destroy.notify = onDecorationDestroy;
    wl_signal_add(&decoration->events.destroy, &watch->destroy);
    applyXdgDecorationMode(watch);
    (void)self;
  }

  void Server::onNewLegacyDecoration(wl_listener*, void* data) {
    auto* decoration = static_cast<wlr_server_decoration*>(data);
    auto* watch = new LegacyDecorationWatch{.decoration = decoration};
    watch->mode.notify = onLegacyDecorationMode;
    wl_signal_add(&decoration->events.mode, &watch->mode);
    watch->destroy.notify = onLegacyDecorationDestroy;
    wl_signal_add(&decoration->events.destroy, &watch->destroy);
    applyLegacyDecorationMode(decoration);
  }

  void Server::onRequestActivate(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_requestActivate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    wlr_xdg_activation_token_v1* token = event->token;
    const char* tokenName = token != nullptr ? wlr_xdg_activation_token_v1_get_name(token) : nullptr;
    const auto* watch = token != nullptr ? static_cast<ActivationTokenWatch*>(token->data) : nullptr;
    const bool trusted = watch != nullptr && (watch->compositorIssued || watch->inputBacked);
    const auto age = watch != nullptr
        ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watch->createdAt)
              .count()
        : -1;
    View* source = token != nullptr ? viewForSurface(*self, token->surface) : nullptr;
    View* target = viewForSurface(*self, event->surface);
    Workspace* targetWorkspace = target != nullptr ? target->workspace() : nullptr;
    const bool targetKeyboardFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    const bool targetPointerFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    kLog.debug(
        "xdg-activation activate token='{}' age_ms={} serial={} seat={} source_surface={} source_pid={} "
        "source_app_id='{}' app_id_hint='{}' target_surface={} target_pid={} target_app_id='{}' mapped={} "
        "visible={} keyboard_focused={} pointer_focused={} workspace='{}' workspace_active={} other_workspace={} "
        "locked={}",
        tokenName != nullptr ? tokenName : "<unknown>", age, token != nullptr ? token->serial : 0,
        static_cast<const void*>(token != nullptr ? token->seat : nullptr),
        static_cast<const void*>(token != nullptr ? token->surface : nullptr),
        surfaceClientPid(token != nullptr ? token->surface : nullptr),
        source != nullptr && source->appId() != nullptr ? source->appId() : "",
        token != nullptr && token->app_id != nullptr ? token->app_id : "", static_cast<const void*>(event->surface),
        surfaceClientPid(event->surface), target != nullptr && target->appId() != nullptr ? target->appId() : "",
        target != nullptr && target->mapped(), target != nullptr && target->onActiveWorkspace(), targetKeyboardFocused,
        targetPointerFocused, targetWorkspace != nullptr ? targetWorkspace->name() : "",
        targetWorkspace != nullptr && targetWorkspace->active(),
        targetWorkspace != nullptr && !targetWorkspace->active(), self->m_sessionLocked
    );
    if (target != nullptr && target->mapped() && tokenName != nullptr) {
      self->endLaunchBusy(std::string(tokenName)); // the launch raised a running app
    }
    bool launchStartup = false;
    if (target != nullptr && token != nullptr) {
      const bool assigned = self->claimLaunchOrigin(*target, token);
      const bool associated = tokenName != nullptr && target->consumeLaunchActivation(tokenName);
      launchStartup = assigned || associated;
    }
    if (launchStartup && target != nullptr && !target->mapped()) {
      target->deferActivation(trusted);
      kLog.debug(
          "xdg-activation startup placement target_app_id='{}' mapped={} action=defer",
          target != nullptr && target->appId() != nullptr ? target->appId() : "", target != nullptr && target->mapped()
      );
      return;
    }
    if (self->m_sessionLocked) {
      return;
    }

    wlr_surface* root = wlr_surface_get_root_surface(event->surface);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
    if (toplevel == nullptr) {
      return;
    }

    for (const auto& entry : self->m_registry.all()) {
      if (entry->toplevel() == toplevel) {
        const std::optional<bool> rulePolicy = entry->resolvedRules().focusOnActivate;
        const bool focusOnActivate = rulePolicy.value_or(trusted || config().general.focusOnActivate);
        Output* preferredOutput = self->outputFromWlr(self->preferredOutput());
        Workspace* preferredWorkspace = preferredOutput != nullptr && preferredOutput->workspaceGroup() != nullptr
            ? preferredOutput->workspaceGroup()->active()
            : nullptr;
        const bool launchAway = launchStartup
            && (self->m_scratchpadManager != nullptr && self->m_scratchpadManager->contains(entry.get())
                    ? self->m_scratchpadManager->activeOn(preferredOutput)
                        != self->m_scratchpadManager->nameFor(entry.get())
                    : entry->workspace() != preferredWorkspace);
        const bool alreadyFocused = entry->activated();
        kLog.debug(
            "xdg-activation policy target_app_id='{}' trusted={} compositor_issued={} input_backed={} mapped={} "
            "focus_on_activate={} already_focused={} action={}",
            entry->appId() != nullptr ? entry->appId() : "", trusted, watch != nullptr && watch->compositorIssued,
            watch != nullptr && watch->inputBacked, entry->mapped(), focusOnActivate, alreadyFocused,
            alreadyFocused ? "none"
                           : (entry->mapped() ? (focusOnActivate && !launchAway ? "focus" : "urgent") : "defer")
        );
        if (alreadyFocused) {
          entry->setUrgent(false);
        } else if (!entry->mapped()) {
          entry->deferActivation(trusted);
        } else if (focusOnActivate && !launchAway) {
          self->focusView(entry.get(), FocusReason::XdgActivation);
        } else {
          entry->setUrgent(true);
        }
        return;
      }
    }
  }

  void Server::onNewActivationToken(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newActivationToken);
    auto* token = static_cast<wlr_xdg_activation_token_v1*>(data);
    self->trackActivationToken(token, false);

    View* source = viewForSurface(*self, token->surface);
    const char* tokenName = wlr_xdg_activation_token_v1_get_name(token);
    kLog.debug(
        "xdg-activation token token='{}' serial={} seat={} source_surface={} source_pid={} source_app_id='{}' "
        "app_id_hint='{}' source_mapped={} source_visible={} source_keyboard_focused={} source_pointer_focused={}",
        tokenName != nullptr ? tokenName : "<unknown>", token->serial, static_cast<const void*>(token->seat),
        static_cast<const void*>(token->surface), surfaceClientPid(token->surface),
        source != nullptr && source->appId() != nullptr ? source->appId() : "",
        token->app_id != nullptr ? token->app_id : "", source != nullptr && source->mapped(),
        source != nullptr && source->onActiveWorkspace(),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface)
    );
  }

  void Server::beginLaunchBusy(std::string token, std::string appId) {
    constexpr auto kBusyLimit = std::chrono::seconds(5);
    m_launchBusy.push_back(
        {.token = std::move(token), .appId = std::move(appId), .deadline = std::chrono::steady_clock::now() + kBusyLimit}
    );
    syncLaunchBusy();
  }

  // token: the launch token of the window; a window without one ends the oldest pending launch.
  void Server::endLaunchBusy(const std::optional<std::string>& token) {
    if (token && m_cpuBoost != nullptr) {
      m_cpuBoost->launchMapped(*token);
    }
    const auto match = token ? std::ranges::find(m_launchBusy, *token, &LaunchBusy::token) : m_launchBusy.begin();
    if (match == m_launchBusy.end()) {
      return;
    }
    m_launchBusy.erase(match);
    syncLaunchBusy();
  }

  void Server::syncLaunchBusy() {
    const auto now = std::chrono::steady_clock::now();
    // A launch without a window by its deadline loses its launch CPU weight.
    std::erase_if(m_launchBusy, [this, now](const LaunchBusy& busy) {
      if (busy.deadline > now) {
        return false;
      }
      if (m_cpuBoost != nullptr) {
        m_cpuBoost->expireLaunch(busy.appId);
      }
      return true;
    });
    if (m_launchBusyTimer != nullptr) {
      int delayMs = 0;
      if (!m_launchBusy.empty()) {
        const auto next = std::ranges::min(m_launchBusy, {}, &LaunchBusy::deadline).deadline;
        delayMs = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(next - now).count()) + 1;
      }
      wl_event_source_timer_update(m_launchBusyTimer, delayMs);
    }
    if (m_cursor != nullptr) {
      m_cursor->setLaunchBusy(!m_launchBusy.empty());
    }
  }

  int Server::onLaunchBusyTimer(void* data) {
    static_cast<Server*>(data)->syncLaunchBusy();
    return 0;
  }

  void Server::trackActivationToken(wlr_xdg_activation_token_v1* token, bool compositorIssued) {
    auto* watch = new ActivationTokenWatch();
    watch->server = this;
    watch->createdAt = std::chrono::steady_clock::now();
    watch->compositorIssued = compositorIssued;
    watch->inputBacked = !compositorIssued && token->seat != nullptr && token->surface != nullptr;
    watch->pointerBacked = watch->inputBacked && token->seat->pointer_state.grab_serial == token->serial;
    if (const char* tokenName = wlr_xdg_activation_token_v1_get_name(token)) {
      watch->tokenName = tokenName;
    }

    Workspace* launchWorkspace = nullptr;
    if (compositorIssued) {
      if (Output* output = outputFromWlr(preferredOutput()); output != nullptr && output->workspaceGroup() != nullptr) {
        launchWorkspace = output->workspaceGroup()->active();
        if (m_scratchpadManager != nullptr) {
          watch->launchScratchpad = m_scratchpadManager->activeOn(output);
        }
      }
    } else if (watch->inputBacked) {
      if (View* source = viewForSurface(*this, token->surface); source != nullptr) {
        if (m_scratchpadManager != nullptr) {
          watch->launchScratchpad = m_scratchpadManager->nameFor(source);
        }
        launchWorkspace = source->workspace();
        if (source->pinned() || launchWorkspace == nullptr) {
          if (Output* output = source->currentOutput(); output != nullptr && output->workspaceGroup() != nullptr) {
            launchWorkspace = output->workspaceGroup()->active();
          }
        }
      } else if (
          LayerSurface* source = LayerSurface::fromSurface(token->surface);
          source != nullptr && source->output() != nullptr && source->output()->workspaceGroup() != nullptr
      ) {
        launchWorkspace = source->output()->workspaceGroup()->active();
        if (m_scratchpadManager != nullptr) {
          watch->launchScratchpad = m_scratchpadManager->activeOn(source->output());
        }
      }
    }
    if (launchWorkspace != nullptr && launchWorkspace->group() != nullptr) {
      watch->launchWorkspace = launchWorkspace->reserveForLaunch();
      watch->launchWorkspaceId = launchWorkspace->id();
      if (Output* output = launchWorkspace->group()->output(); output != nullptr && output->wlr()->name != nullptr) {
        watch->launchOutputName = output->wlr()->name;
      }
    }
    // A launch shows the busy cursor: the shell's dock and launcher set the app id hint. Compositor spawns do not:
    // most spawn keybinds run commands without a window (volume, brightness).
    if (!watch->tokenName.empty() && token->app_id != nullptr) {
      beginLaunchBusy(watch->tokenName, token->app_id);
    }
    watch->destroy.notify = onActivationTokenDestroy;
    wl_signal_add(&token->events.destroy, &watch->destroy);
    token->data = watch;
  }

  bool Server::claimLaunchOrigin(View& view, wlr_xdg_activation_token_v1* token) {
    auto* watch = token != nullptr ? static_cast<ActivationTokenWatch*>(token->data) : nullptr;
    if (watch == nullptr || watch->launchClaimed || watch->launchWorkspace == nullptr) {
      return false;
    }
    watch->launchClaimed = true;
    std::shared_ptr<WorkspaceLaunchAnchor> launchWorkspace = watch->launchWorkspace;
    Workspace* workspace = launchWorkspace->workspace;
    WorkspaceGroup* sourceGroup = workspace != nullptr ? workspace->group() : nullptr;
    WorkspaceGroup* targetGroup = sourceGroup;
    std::string launchOutputName = watch->launchOutputName;
    std::string launchWorkspaceId = watch->launchWorkspaceId;
    bool pointerDrop = false;
    if (watch->launchScratchpad.empty()
        && watch->pointerBacked
        && sameWaylandClient(token->surface, view.rootSurface())) {
      Output* output = outputFromWlr(preferredOutput());
      Workspace* pointerWorkspace =
          output != nullptr && output->workspaceGroup() != nullptr ? output->workspaceGroup()->active() : nullptr;
      if (pointerWorkspace != nullptr
          && pointerWorkspace->group() != nullptr
          && pointerWorkspace->group() != sourceGroup) {
        launchWorkspace = pointerWorkspace->reserveForLaunch();
        workspace = pointerWorkspace;
        targetGroup = pointerWorkspace->group();
        launchWorkspaceId = pointerWorkspace->id();
        launchOutputName = output->wlr()->name != nullptr ? output->wlr()->name : "";
        pointerDrop = true;
      }
    }
    const char* tokenName = wlr_xdg_activation_token_v1_get_name(token);
    uint32_t timeoutMsec = 0;
    if (token->activation != nullptr && token->activation->token_timeout_msec > 0) {
      const auto age =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watch->createdAt)
              .count();
      const uint32_t configured = token->activation->token_timeout_msec;
      timeoutMsec = age >= 0 && static_cast<uint64_t>(age) < configured ? configured - static_cast<uint32_t>(age) : 1;
    }
    const bool assigned = workspace != nullptr
        && tokenName != nullptr
        && view.assignLaunchOrigin(tokenName, std::move(launchWorkspace), timeoutMsec, watch->launchScratchpad);
    watch->launchWorkspace.reset();
    if (sourceGroup != nullptr && !m_stopping) {
      sourceGroup->reconcileDynamic();
    }
    if (targetGroup != nullptr && targetGroup != sourceGroup && !m_stopping) {
      targetGroup->reconcileDynamic();
    }
    kLog.debug(
        "launch placement token='{}' output='{}' workspace='{}' scratchpad='{}' target_app_id='{}' pointer_drop={} "
        "assigned={}",
        tokenName != nullptr ? tokenName : "<unknown>", launchOutputName, launchWorkspaceId, watch->launchScratchpad,
        view.appId() != nullptr ? view.appId() : "", pointerDrop, assigned
    );
    return assigned;
  }

  void Server::assignLaunchOriginFromEnvironment(View& view) {
    if (m_xdgActivation == nullptr || view.pid() <= 0) {
      return;
    }
    const std::optional<std::string> tokenName = processEnvironmentValue(view.pid(), kLaunchTokenEnvironment);
    if (!tokenName || tokenName->empty()) {
      return;
    }
    wlr_xdg_activation_token_v1* token = wlr_xdg_activation_v1_find_token(m_xdgActivation, tokenName->c_str());
    const auto* watch = token != nullptr ? static_cast<ActivationTokenWatch*>(token->data) : nullptr;
    if (watch == nullptr || !watch->compositorIssued) {
      return;
    }
    claimLaunchOrigin(view, token);
  }

  void Server::onActivationTokenDestroy(wl_listener* listener, void* /*data*/) {
    ActivationTokenWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Workspace* workspace = watch->launchWorkspace != nullptr ? watch->launchWorkspace->workspace : nullptr;
    WorkspaceGroup* group = workspace != nullptr ? workspace->group() : nullptr;
    Server* server = watch->server;
    const std::string tokenName = watch->tokenName;
    watch->launchWorkspace.reset();
    wl_list_remove(&watch->destroy.link);
    delete watch;
    if (server != nullptr && !tokenName.empty()) {
      for (const auto& entry : server->m_registry.all()) {
        if (entry->cancelLaunchOrigin(tokenName)) {
          break;
        }
      }
    }
    if (group != nullptr && server != nullptr && !server->m_stopping) {
      group->reconcileDynamic();
    }
  }

  void Server::removeView(View* view) {
    if (view == nullptr) {
      return;
    }
    view->invalidateRefs();
    for (const auto& output : m_outputs) {
      output->onViewRemoved(view);
    }
    const bool hadKeyboardFocus = m_seat->wlr()->keyboard_state.focused_surface == view->rootSurface();
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->remove(view);
    }
    View* replacement = nullptr;
    Output* output = nullptr;
    if (Workspace* workspace = view->workspace()) {
      if (workspace->group() != nullptr) {
        output = workspace->group()->output();
      }
      replacement = workspace->removeView(view);
      view->detachWorkspace();
    }
    const bool restoreAfterRemoval = view->displacedHome().has_value();
    m_registry.remove(view);
    if (restoreAfterRemoval) {
      scheduleDisplacedViewRestore();
    }
    if (hadKeyboardFocus) {
      notifyKeyboardClearFocus();
      if (replacement != nullptr) {
        focusView(replacement, FocusReason::Directional);
      } else {
        refocus(output);
      }
    }
  }

  void Server::onToplevelCaptureRequest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_toplevelCaptureRequest);
    auto* request = static_cast<wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*>(data);

    if (request->toplevel_handle == nullptr || request->toplevel_handle->data == nullptr) {
      return;
    }
    auto* view = static_cast<View*>(request->toplevel_handle->data);

    // Reuse a cached capture source when one already exists for this view.
    if (view->m_captureSource == nullptr) {
      wlr_scene_tree* captureTree = view->captureTree();
      if (captureTree == nullptr) {
        return;
      }
      view->m_captureSource = wlr_ext_image_capture_source_v1_create_with_scene_node(
          &captureTree->node, wl_display_get_event_loop(self->m_display), self->m_allocator, self->m_renderer
      );
      if (view->m_captureSource == nullptr) {
        return;
      }
      view->m_captureSourceDestroy.notify = View::onCaptureSourceDestroy;
      wl_signal_add(&view->m_captureSource->events.destroy, &view->m_captureSourceDestroy);
    }

    if (Output* output = view->currentOutput()) {
      andromedafx_scene_capture_source_set_scale(view->m_captureSource, output->wlr()->scale);
    }

    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, view->m_captureSource);
  }

} // namespace andromeda
