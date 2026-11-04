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
#include "core/screen_activity_recorder.h"
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

    bool outputStateMatchesCurrentBackend(const wlr_output_state& state, const wlr_output& output) {
      if ((state.committed & WLR_OUTPUT_STATE_MODE) != 0) {
        if (state.mode_type == WLR_OUTPUT_STATE_MODE_FIXED) {
          if (state.mode != output.current_mode) {
            return false;
          }
        } else if (
            output.current_mode != nullptr
            || state.custom_mode.width != output.width
            || state.custom_mode.height != output.height
            || state.custom_mode.refresh != output.refresh
        ) {
          return false;
        }
      }
      if ((state.committed & WLR_OUTPUT_STATE_SCALE) != 0
          && wl_fixed_from_double(state.scale) != wl_fixed_from_double(output.scale)) {
        return false;
      }
      if ((state.committed & WLR_OUTPUT_STATE_TRANSFORM) != 0 && state.transform != output.transform) {
        return false;
      }
      const bool adaptiveSyncEnabled = output.adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
      return (state.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) == 0
          || state.adaptive_sync_enabled == adaptiveSyncEnabled;
    }
  } // namespace

  void Server::onNewOutput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newOutput);
    self->addOutput(static_cast<wlr_output*>(data));
  }

  void Server::onSetGamma(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_setGamma);
    auto* event = static_cast<wlr_gamma_control_manager_v1_set_gamma_event*>(data);
    if (Output* out = self->outputFromWlr(event->output)) {
      out->onGammaChanged(event->control);
    }
  }

  void Server::addOutput(wlr_output* output) {
    if (!m_pendingOutputName.empty()) {
      // Before the Output exists: adding it to the layout advertises the name to clients, and it cannot change after.
      wlr_output_set_name(output, m_pendingOutputName.c_str());
    }
    m_outputs.push_back(std::make_unique<Output>(*this, output));
    applyConfiguredMirrors();
    scheduleDisplacedViewRestore();
    markDirty(Dirty::Backdrop | Dirty::Banner | Dirty::QuitConfirm);
    if (m_sessionLocked) {
      updateLockBlank();
      raiseLockTree();
    }
    if (m_sessionLock != nullptr) {
      m_sessionLock->outputsChanged();
    }
    updateOutputManagerConfig();
    refreshSurfaceScales();
  }

  void Server::removeOutput(Output* output) {
    for (const auto& entry : m_outputs) {
      if (entry->mirrorSource() == output) {
        entry->setMirrorSource(nullptr);
      }
    }
    const bool hadRuntimeEffect = output->screenEffectSlot().runtimeSelector.has_value();
    m_overview->onOutputRemoved(output);
    output->invalidateRefs();
    m_gestures->cancelForOutput(output);
    if (!m_cursor->isPassthrough()) {
      m_cursor->resetMode();
    }
    m_effects.removeOutput(output);
    if (m_insertHint != nullptr && m_insertHint->output() == output) {
      m_insertHint->hideImmediate();
    }
    // Dropping the snapshot unregisters it and destroys its scene tree.
    std::erase_if(m_closeSnapshots, [this, output](const std::unique_ptr<CloseSnapshot>& snap) {
      if (!snap->animatesOn(output)) {
        return false;
      }
      unregisterAnimatable(snap.get());
      return true;
    });
    // wlroots 0.20 does not track output lifetime for layer surfaces, so their wlr_output pointer would dangle once the
    // output is freed. Destroy them now while the wlr_output is still valid.
    closeLayerSurfaces(output->wlr());

    Output* fallback = nullptr;
    for (const auto& entry : m_outputs) {
      // Prefer a live monitor: rehoming windows onto a disabled one would
      // strand them off-screen.
      if (entry.get() != output && entry->onDesktop()) {
        fallback = entry.get();
        break;
      }
    }

    reassignOutputViews(output, fallback);
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->releaseOutput(output);
    }

    if (m_sessionLock != nullptr) {
      m_sessionLock->forgetOutput(output->wlr());
    }
    std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) { return entry.get() == output; });
    if (hadRuntimeEffect && !m_stopping) {
      m_effects.prepare(m_renderer);
    }
    markDirty(Dirty::Banner | Dirty::QuitConfirm);
    if (m_sessionLocked) {
      updateLockBlank();
    }
    if (m_sessionLock != nullptr) {
      m_sessionLock->outputsChanged();
    }
    updateOutputManagerConfig();
    // Scratchpad and pinned views rehome without going through setWorkspace.
    refreshSurfaceScales();
  }

  void Server::reassignOutputViews(Output* source, Output* destination, bool joinActive) {
    if (source == nullptr || source == destination) {
      return;
    }
    WorkspaceGroup* sourceGroup = source->workspaceGroup();
    Workspace* targetWorkspace = destination != nullptr && destination->workspaceGroup() != nullptr
        ? destination->workspaceGroup()->active()
        : nullptr;
    if (sourceGroup != nullptr) {
      for (size_t index = 0; index < sourceGroup->workspaceCount(); ++index) {
        sourceGroup->workspaceAt(index)->invalidateLaunchReservations();
      }
    }
    const char* sourceName = source->wlr()->name;
    if (sourceGroup != nullptr
        && sourceGroup->active() != nullptr
        && !sourceGroup->active()->displacedWorkspace()
        && sourceName != nullptr) {
      const auto existing = std::ranges::find_if(m_displacedWorkspaceSelections, [sourceName](const auto& selection) {
        return selection.outputName == sourceName;
      });
      if (existing == m_displacedWorkspaceSelections.end()) {
        m_displacedWorkspaceSelections.push_back({
            .outputName = sourceName,
            .workspaceName = sourceGroup->active()->name(),
            .workspaceIndex = sourceGroup->active()->index(),
            .workspaceNamed = sourceGroup->active()->named(),
        });
      }
    }
    const auto rememberWorkspace = [this](Workspace* workspace, Output* output, bool tiledOnly) {
      std::shared_ptr<const DisplacedWorkspace> origin = workspace->displacedWorkspace();
      if (!origin) {
        const OutputIdentity identity = output->identity();
        origin = std::make_shared<const DisplacedWorkspace>(DisplacedWorkspace{
            .outputName = std::string(identity.connector),
            .outputMake = std::string(identity.make),
            .outputModel = std::string(identity.model),
            .outputSerial = std::string(identity.serial),
            .workspaceName = workspace->name(),
            .workspaceIndex = workspace->index(),
            .workspaceNamed = workspace->named(),
            .configGeneration = configStore().generation(),
            .layoutModeOverride = workspace->layoutModeOverride(),
        });
      }
      const wlr_box outputBox = output->layoutBox();
      const LayoutCapture capture = workspace->scrollingLayout() != nullptr
          ? workspace->scrollingLayout()->captureStateForViewport(workspace->scrollViewportExtent())
          : workspace->layout().captureState();
      for (const auto& view : m_registry.placementRoots()) {
        if (view->workspace() != workspace || view->displacedHome()) {
          continue;
        }
        const auto member = std::ranges::find_if(capture.members, [view = view.get()](const LayoutMember& entry) {
          return entry.view == view;
        });
        if (tiledOnly && member == capture.members.end()) {
          continue;
        }
        if (view->floating()) {
          view->rememberFloatingPosition();
        }
        View::DisplacedHome home{
            .outputName = origin->outputName,
            .workspaceName = origin->workspaceName,
            .workspaceIndex = origin->workspaceIndex,
            .workspaceNamed = origin->workspaceNamed,
            .layoutSnapshot = nullptr,
            .layoutMember = 0,
            .ownsNamedScrollingColumnExtent = view->m_ownsNamedScrollingColumnExtent,
            .pendingNamedScrollingColumnExtentPx = std::nullopt,
            .pendingNamedScrollingColumnExtent = std::nullopt,
            .layoutModeOverride = workspace->layoutModeOverride(),
            .floatingOutputPosition = std::nullopt,
            .configGeneration = configStore().generation(),
            .layoutProtectionOnly = tiledOnly,
            .workspaceOrigin = origin,
        };
        if (view->floating() && outputBox.width > 0 && outputBox.height > 0) {
          home.floatingOutputPosition = {{
              static_cast<double>(view->layoutTargetX() - outputBox.x) / outputBox.width,
              static_cast<double>(view->layoutTargetY() - outputBox.y) / outputBox.height,
          }};
        }
        if (member != capture.members.end()) {
          home.layoutSnapshot = capture.snapshot;
          home.layoutMember = member->id;
        }
        view->markDisplaced(std::move(home));
      }
      return origin;
    };

    if (sourceGroup != nullptr) {
      std::vector<View*> leaving;
      for (const auto& view : m_registry.placementRoots()) {
        Workspace* workspace = view->workspace();
        if (workspace == nullptr || workspace->group() != sourceGroup) {
          continue;
        }
        leaving.push_back(view.get());
      }

      struct EvacuatedWorkspace {
        std::shared_ptr<WorkspaceLaunchAnchor> source;
        std::shared_ptr<const DisplacedWorkspace> origin;
        ResolvedLayoutConfig layout;
        std::optional<LayoutMode> layoutOverride;
        LayoutCapture capture;
        std::vector<View*> views;
        View* focused = nullptr;
      };
      std::vector<EvacuatedWorkspace> workspaces;
      // Capture every source before moving a view. Reservation anchors keep
      // dynamic reconciliation from deleting the batches still being moved.
      for (size_t index = 0; index < sourceGroup->workspaceCount(); ++index) {
        Workspace* workspace = sourceGroup->workspaceAt(index);
        refreshDisplacedWorkspace(*workspace);
        EvacuatedWorkspace batch{
            .source = workspace->reserveForLaunch(),
            .origin = rememberWorkspace(workspace, source, false),
            .layout = workspace->layoutConfig(),
            .layoutOverride = workspace->layoutModeOverride(),
            .capture = workspace->scrollingLayout() != nullptr
                ? workspace->scrollingLayout()->captureStateForViewport(workspace->scrollViewportExtent())
                : workspace->layout().captureState(),
            .views = {},
            .focused = workspace->focusedView(),
        };
        for (View* view : leaving) {
          if (view->workspace() == workspace) {
            batch.views.push_back(view);
          }
        }
        if (!batch.views.empty()) {
          workspaces.push_back(std::move(batch));
        }
      }

      bool protectedFallback = false;
      for (const EvacuatedWorkspace& batch : workspaces) {
        const bool hasOrdinaryViews =
            std::ranges::any_of(batch.views, [](const View* view) { return !view->pinned(); });
        const bool join = joinActive && targetWorkspace != nullptr;
        Workspace* refuge = hasOrdinaryViews && !join
            ? displacedWorkspaceDestination(batch.origin, destination, source)
            : targetWorkspace;
        // A joined active workspace keeps its own layout, even when it is itself a refuge.
        const bool separate = !join && refuge != nullptr && refuge->displacedWorkspace() != nullptr;
        const auto hold = refuge != nullptr ? refuge->reserveForLaunch() : nullptr;
        bool capturedLayout = false;
        if (separate) {
          refuge->applyLayoutConfig(batch.layout);
          if (batch.layoutOverride) {
            refuge->overrideLayoutMode(*batch.layoutOverride);
          }
          capturedLayout = batch.capture.snapshot != nullptr
              && refuge->layout().restoreState(*batch.capture.snapshot, batch.capture.members);
        } else if (refuge != nullptr && !protectedFallback) {
          // Only exhausted inventories need to mix with a native workspace.
          // Keep its clean layout available for the existing restore path.
          rememberWorkspace(refuge, refuge->group()->output(), true);
          protectedFallback = true;
        }
        for (View* view : batch.views) {
          if (view->displacedHome() && view->displacedHome()->layoutProtectionOnly) {
            View::DisplacedHome home = *view->displacedHome();
            home.layoutProtectionOnly = false;
            view->markDisplaced(std::move(home));
          }
          const bool floating = view->floating();
          Workspace* target = view->pinned() ? targetWorkspace : refuge;
          view->setWorkspace(target, !capturedLayout || target != refuge);
          if (floating && target != nullptr) {
            view->restoreFloatingPosition(false);
          }
        }
        if (separate) {
          refuge->setFocusedView(batch.focused);
          if (capturedLayout && refuge->scrollingLayout() != nullptr) {
            refuge->scrollingLayout()->restoreSnapshotViewport(
                *batch.capture.snapshot, refuge->scrollViewportExtent(), false
            );
          }
          refuge->markArrange(false);
        }
      }
    }
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->moveOutput(source, destination);
    }
    if (sourceGroup != nullptr) {
      // A disabled output draws no frame to finish its now-empty layouts.
      sourceGroup->reconcileDynamic();
      sourceGroup->flushArrange();
    }
  }

  void Server::refreshDisplacedWorkspace(Workspace& workspace) {
    const auto& origin = workspace.displacedWorkspace();
    if (!origin) {
      return;
    }
    const auto belongsToOrigin = [&origin](const View::DisplacedHome& home) {
      return home.outputName == origin->outputName
          && home.workspaceName == origin->workspaceName
          && home.workspaceIndex == origin->workspaceIndex
          && home.workspaceNamed == origin->workspaceNamed;
    };
    // An unmapped peer still needs its slot in the original complete snapshot.
    // Otherwise the live temporary layout is authoritative, including edits
    // and new members made while the original output was absent.
    if (std::ranges::any_of(m_registry.all(), [&belongsToOrigin](const auto& view) {
          return !view->mapped()
              && view->displacedHome()
              && view->displacedHome()->layoutSnapshot
              && belongsToOrigin(*view->displacedHome());
        })) {
      return;
    }
    const LayoutCapture capture = workspace.scrollingLayout() != nullptr
        ? workspace.scrollingLayout()->captureStateForViewport(workspace.scrollViewportExtent())
        : workspace.layout().captureState();
    for (const LayoutMember& member : capture.members) {
      View* view = member.view;
      if (view == nullptr || !view->mapped() || !view->displacedHome() || !belongsToOrigin(*view->displacedHome())) {
        continue;
      }
      View::DisplacedHome home = *view->displacedHome();
      home.layoutSnapshot = capture.snapshot;
      home.layoutMember = member.id;
      home.layoutModeOverride = workspace.layoutModeOverride();
      home.configGeneration = configStore().generation();
      home.ownsNamedScrollingColumnExtent = view->m_ownsNamedScrollingColumnExtent;
      view->markDisplaced(std::move(home));
    }
  }

  Workspace* Server::displacedWorkspaceDestination(
      const std::shared_ptr<const DisplacedWorkspace>& home, Output* preferred, Output* excluded
  ) {
    const auto onOutput = [&](Output* output) -> Workspace* {
      if (output == nullptr
          || output == excluded
          || !output->desktopEnabled()
          || !output->wlr()->enabled
          || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->displacedDestination(home);
    };
    if (Workspace* workspace = onOutput(preferred)) {
      return workspace;
    }
    for (const auto& output : m_outputs) {
      if (output.get() != preferred) {
        if (Workspace* workspace = onOutput(output.get())) {
          return workspace;
        }
      }
    }
    if (preferred != nullptr
        && preferred != excluded
        && preferred->wlr()->enabled
        && preferred->workspaceGroup() != nullptr) {
      kLog.warn(
          "all enabled outputs reached the workspace limit; merging displaced workspace {} from {}",
          home->workspaceName, home->outputName
      );
      WorkspaceGroup* group = preferred->workspaceGroup();
      if (!group->active()->displacedWorkspace()) {
        return group->active();
      }
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        if (!group->workspaceAt(index)->displacedWorkspace()) {
          return group->workspaceAt(index);
        }
      }
    }
    return nullptr;
  }

  // Deferred to idle: outputs come back one at a time, and a returning one has no workspace group until addOutput is
  // done with it.
  void Server::scheduleDisplacedViewRestore() {
    if (m_displacedRestoreIdle != nullptr) {
      return;
    }
    m_displacedRestoreIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onDisplacedRestoreIdle, this);
    if (m_displacedRestoreIdle == nullptr) {
      kLog.error("failed to register displaced window restore idle source");
    }
  }

  void Server::onDisplacedRestoreIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_displacedRestoreIdle = nullptr;
    server->restoreDisplacedViews();
  }

  void Server::restoreDisplacedViews() {
    Output* fallback = outputFromWlr(preferredOutput());
    if (fallback == nullptr) {
      return;
    }
    const auto nativeFallback = [](WorkspaceGroup* group) -> Workspace* {
      if (group->active() != nullptr && !group->active()->displacedWorkspace()) {
        return group->active();
      }
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        Workspace* workspace = group->workspaceAt(index);
        if (!workspace->displacedWorkspace()) {
          return workspace;
        }
      }
      return nullptr;
    };
    for (const auto& output : m_outputs) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        for (size_t index = 0; index < group->workspaceCount(); ++index) {
          refreshDisplacedWorkspace(*group->workspaceAt(index));
        }
      }
    }
    std::vector<View*> displaced;
    for (const auto& entry : m_registry.all()) {
      if (entry->displacedHome()) {
        displaced.push_back(entry.get());
      }
    }
    const auto homeIsAvailable = [this](const View* view) {
      const View::DisplacedHome& home = *view->displacedHome();
      Output* output = outputFromName(home.outputName);
      return output != nullptr && output->workspaceGroup() != nullptr;
    };
    // Available homes go first, before refugees can occupy their layouts.
    // Within an output, resolving and populating numbered workspaces in order
    // lets a dynamic group create workspace N+1 before it is requested.
    std::ranges::sort(displaced, [homeIsAvailable](const View* lhs, const View* rhs) {
      const View::DisplacedHome& left = *lhs->displacedHome();
      const View::DisplacedHome& right = *rhs->displacedHome();
      const bool leftAvailable = homeIsAvailable(lhs);
      const bool rightAvailable = homeIsAvailable(rhs);
      if (leftAvailable != rightAvailable) {
        return leftAvailable;
      }
      if (leftAvailable && left.layoutProtectionOnly != right.layoutProtectionOnly) {
        return !left.layoutProtectionOnly;
      }
      if (left.outputName != right.outputName) {
        return left.outputName < right.outputName;
      }
      return left.workspaceIndex != right.workspaceIndex ? left.workspaceIndex < right.workspaceIndex
                                                         : left.workspaceName < right.workspaceName;
    });

    struct RestoredViewport {
      std::shared_ptr<const LayoutSnapshot> snapshot;
      Workspace* workspace = nullptr;
      bool geometryUnchanged = false;
    };
    std::vector<RestoredViewport> restoredViewports;
    size_t restored = 0;

    for (size_t first = 0; first < displaced.size();) {
      const View::DisplacedHome groupHome = *displaced[first]->displacedHome();
      size_t last = first + 1;
      while (last < displaced.size()) {
        const View::DisplacedHome& candidate = *displaced[last]->displacedHome();
        if (candidate.outputName != groupHome.outputName
            || candidate.workspaceName != groupHome.workspaceName
            || candidate.workspaceIndex != groupHome.workspaceIndex
            || candidate.workspaceNamed != groupHome.workspaceNamed
            || candidate.layoutProtectionOnly != groupHome.layoutProtectionOnly) {
          break;
        }
        ++last;
      }

      Output* target = outputFromName(groupHome.outputName);
      WorkspaceGroup* targetGroup = target != nullptr ? target->workspaceGroup() : nullptr;
      const bool homeAvailable = targetGroup != nullptr;
      Workspace* workspace = nullptr;
      std::shared_ptr<WorkspaceLaunchAnchor> refugeHold;
      if (!homeAvailable) {
        for (size_t index = first; index < last; ++index) {
          View* view = displaced[index];
          if (view->pinned() && view->workspace() == nullptr) {
            view->setWorkspace(fallback->workspaceGroup()->active());
            view->restoreFloatingPosition(false);
            ++restored;
          }
        }
        const bool needsRefuge = std::ranges::any_of(
            displaced.begin() + static_cast<std::ptrdiff_t>(first),
            displaced.begin() + static_cast<std::ptrdiff_t>(last),
            [](const View* view) { return !view->pinned() && view->workspace() == nullptr; }
        );
        if (!needsRefuge) {
          first = last;
          continue;
        }
        workspace = groupHome.workspaceOrigin ? displacedWorkspaceDestination(groupHome.workspaceOrigin, fallback)
                                              : fallback->workspaceGroup()->active();
        if (workspace == nullptr || !workspace->displacedWorkspace()) {
          if (workspace != nullptr) {
            for (size_t index = first; index < last; ++index) {
              View* view = displaced[index];
              if (view->workspace() == nullptr) {
                const bool floating = view->floating();
                view->setWorkspace(workspace);
                if (floating) {
                  view->restoreFloatingPosition(false);
                }
                ++restored;
              }
            }
          }
          first = last;
          continue;
        }
        refugeHold = workspace->reserveForLaunch();
        targetGroup = workspace->group();
        target = targetGroup->output();
      } else if (groupHome.workspaceNamed) {
        workspace = targetGroup->workspaceNamed(groupHome.workspaceName);
      } else {
        if (targetGroup->dynamic()) {
          // A recreated dynamic group starts with its configured names and
          // sentinel. Materialize the saved anonymous position before
          // restoring its surviving windows.
          while (targetGroup->workspaceCount() <= groupHome.workspaceIndex) {
            if (targetGroup->insertDynamicWorkspace(targetGroup->workspaceCount()) == nullptr) {
              break;
            }
          }
        }
        workspace = targetGroup->workspaceAt(groupHome.workspaceIndex);
        if (targetGroup->dynamic() && workspace != nullptr && (workspace->named() || workspace->displacedWorkspace())) {
          workspace = targetGroup->insertDynamicWorkspace(groupHome.workspaceIndex);
        }
      }
      if (workspace != nullptr && workspace->displacedWorkspace() && homeAvailable) {
        workspace = nullptr;
      }
      const bool selectorMatched = workspace != nullptr;
      if (workspace == nullptr) {
        workspace = nativeFallback(targetGroup);
      }
      if (workspace == nullptr) {
        first = last;
        continue;
      }
      const auto applyPendingNamedScrollingColumnExtent = [workspace](View* view, const View::DisplacedHome& home) {
        if ((!home.pendingNamedScrollingColumnExtentPx && !home.pendingNamedScrollingColumnExtent)
            || !view->namedScrollingColumnName()) {
          return;
        }
        ScrollingLayout* scrolling = workspace->scrollingLayout();
        const int column = scrolling != nullptr ? scrolling->columnOf(view) : -1;
        if (column >= 0) {
          if (home.pendingNamedScrollingColumnExtentPx) {
            scrolling->setWidthFromPixels(
                column, workspace->scrollViewportExtent(), *home.pendingNamedScrollingColumnExtentPx
            );
          } else if (home.pendingNamedScrollingColumnExtent) {
            scrolling->setWidthFraction(column, *home.pendingNamedScrollingColumnExtent);
          }
          workspace->markArrange(false);
        }
      };

      struct SnapshotCandidate {
        std::shared_ptr<const LayoutSnapshot> snapshot;
        std::vector<View*> views;
        View::DisplacedHome representative;
      };
      std::vector<SnapshotCandidate> candidates;
      if (selectorMatched) {
        for (size_t index = first; index < last; ++index) {
          View* view = displaced[index];
          const View::DisplacedHome& home = *view->displacedHome();
          if (!view->tiled() || home.layoutSnapshot == nullptr) {
            continue;
          }
          auto candidate = std::ranges::find_if(candidates, [&home](const SnapshotCandidate& entry) {
            return entry.snapshot.get() == home.layoutSnapshot.get();
          });
          if (candidate == candidates.end()) {
            candidates.push_back({
                .snapshot = home.layoutSnapshot,
                .views = {},
                .representative = home,
            });
            candidate = std::prev(candidates.end());
          }
          candidate->views.push_back(view);
        }
      }

      SnapshotCandidate* exact = nullptr;
      for (SnapshotCandidate& candidate : candidates) {
        if (exact == nullptr
            || candidate.views.size() > exact->views.size()
            || (candidate.views.size() == exact->views.size()
                && candidate.snapshot->memberCount() > exact->snapshot->memberCount())) {
          exact = &candidate;
        }
      }
      if (exact != nullptr
          && workspace->layoutMode() != exact->snapshot->mode()
          && exact->representative.layoutModeOverride.has_value()
          && *exact->representative.layoutModeOverride == exact->snapshot->mode()
          && exact->representative.configGeneration == configStore().generation()) {
        workspace->overrideLayoutMode(*exact->representative.layoutModeOverride);
      }
      if (exact != nullptr && workspace->layoutMode() != exact->snapshot->mode()) {
        exact = nullptr;
      }
      const bool snapshotMemberIsUnmapped = exact != nullptr
          && std::ranges::any_of(displaced.begin() + static_cast<std::ptrdiff_t>(first),
                                 displaced.begin() + static_cast<std::ptrdiff_t>(last), [exact](const View* view) {
                                   const auto& home = view->displacedHome();
                                   return !view->mapped()
                                       && home.has_value()
                                       && home->layoutSnapshot.get() == exact->snapshot.get();
                                 });

      std::vector<View*> exactViews;
      if (exact != nullptr) {
        for (View* view : exact->views) {
          const bool ownsNamedScrollingColumnExtent = view->displacedHome()->ownsNamedScrollingColumnExtent;
          view->setWorkspace(workspace, false);
          view->m_ownsNamedScrollingColumnExtent = ownsNamedScrollingColumnExtent;
          if (view->workspace() == workspace && view->mapped() && view->tiled()) {
            exactViews.push_back(view);
          }
        }
      }

      if (exact != nullptr && !exactViews.empty()) {
        std::vector<View*> previousOrder;
        for (const Column& column : workspace->layout().columns()) {
          for (View* view : column.views) {
            if (view != nullptr && std::ranges::find(previousOrder, view) == previousOrder.end()) {
              previousOrder.push_back(view);
            }
          }
        }
        for (View* view : previousOrder) {
          if (view->workspace() == workspace && workspace->layout().columnOf(view) >= 0) {
            workspace->layoutDetach(view, false);
          }
        }

        std::vector<LayoutMember> members;
        members.reserve(exactViews.size());
        for (View* view : exactViews) {
          const View::DisplacedHome& home = *view->displacedHome();
          members.push_back({.id = home.layoutMember, .view = view});
        }
        const bool restoredState = workspace->layout().restoreState(*exact->snapshot, members);
        for (View* view : previousOrder) {
          if (view->workspace() == workspace && view->mapped() && view->tiled()) {
            workspace->layoutAttach(view);
          }
        }
        if (!restoredState) {
          for (View* view : exactViews) {
            if (view->workspace() == workspace && view->mapped() && view->tiled()) {
              workspace->layoutAttach(view);
            }
          }
        } else {
          if (workspace->scrollingLayout() != nullptr) {
            restoredViewports.push_back({
                .snapshot = exact->snapshot,
                .workspace = workspace,
                .geometryUnchanged = exact->representative.configGeneration == configStore().generation(),
            });
          }
          workspace->markArrange(false);
        }
        for (View* view : exactViews) {
          applyPendingNamedScrollingColumnExtent(view, *view->displacedHome());
        }
        restored += exactViews.size();
      }

      const bool foreignRefugeesRemain =
          std::ranges::any_of(m_registry.all(), [&groupHome, workspace](const auto& entry) {
            const auto& home = entry->displacedHome();
            return entry->workspace() == workspace && home.has_value() && home->outputName != groupHome.outputName;
          });
      const bool retainLayoutProtection =
          exact != nullptr && exact->representative.layoutProtectionOnly && foreignRefugeesRemain;
      const auto retainHome = [exact, retainLayoutProtection, snapshotMemberIsUnmapped, selectorMatched, workspace,
                               homeAvailable](const View* view) {
        const auto& home = view->displacedHome();
        if (!home) {
          return false;
        }
        return !homeAvailable
            || (retainLayoutProtection && home->layoutProtectionOnly)
            || (snapshotMemberIsUnmapped && home->layoutSnapshot.get() == exact->snapshot.get())
            || (!view->mapped()
                && selectorMatched
                && home->layoutSnapshot != nullptr
                && workspace->layoutMode() == home->layoutSnapshot->mode());
      };

      for (size_t index = first; index < last; ++index) {
        View* view = displaced[index];
        if (!homeAvailable && view->pinned()) {
          continue;
        }
        if (std::ranges::find(exactViews, view) != exactViews.end()) {
          if (!retainHome(view)) {
            view->clearDisplaced();
          }
          continue;
        }
        const View::DisplacedHome home = *view->displacedHome();
        const bool floating = view->floating();
        if (!retainHome(view)) {
          view->clearDisplaced();
        }
        const bool moved = view->workspace() != workspace;
        if (moved) {
          view->setWorkspace(workspace);
        }
        applyPendingNamedScrollingColumnExtent(view, home);
        if (floating && home.floatingOutputPosition && target != nullptr) {
          const wlr_box outputBox = target->layoutBox();
          if (outputBox.width > 0 && outputBox.height > 0) {
            view->cancelPositionAnimation();
            // The fractions are proportional to the output box; on a smaller output the result can resolve outside
            // the grabbable margin, so the placement is clamped like any other restore.
            const FloatingPoint restored{
                .x = outputBox.x + static_cast<int>(std::lround((*home.floatingOutputPosition)[0] * outputBox.width)),
                .y = outputBox.y + static_cast<int>(std::lround((*home.floatingOutputPosition)[1] * outputBox.height)),
            };
            const FloatingPoint origin = view->grabbableOrigin(restored, view->floatingUsableArea());
            view->setPosition(origin.x, origin.y);
          }
        } else if (floating && moved) {
          view->restoreFloatingPosition(true);
        }
        restored += moved ? 1 : 0;
      }

      first = last;
    }

    if (m_scratchpadManager != nullptr) {
      restored += m_scratchpadManager->restoreDisplaced(fallback);
    }

    // A fixed-size client can finish mapping while every output is absent. It never had a workspace, so it has no
    // displaced home for the restore pass above to find. Scratchpad entries are intentionally workspace-less; attach
    // every other mapped orphan once an output can own it.
    WorkspaceGroup* fallbackGroup = fallback->workspaceGroup();
    Workspace* refuge = fallbackGroup != nullptr ? fallbackGroup->active() : nullptr;
    if (refuge != nullptr) {
      for (const auto& entry : m_registry.placementRoots()) {
        View* view = entry.get();
        if (!view->mapped()
            || view->workspace() != nullptr
            || (m_scratchpadManager != nullptr && m_scratchpadManager->contains(view))) {
          continue;
        }
        const bool floating = view->floating();
        const bool positioned = view->m_positioned;
        const ResolvedWindowRule rule = view->resolvedRules();
        if (!view->attachToAvailableWorkspace(rule, LayoutAttachOrigin::ExistingView)) {
          continue;
        }
        if (floating) {
          if (positioned) {
            view->restoreFloatingPosition(false);
          } else {
            view->placeInUsableArea(rule.defaultPosition);
          }
          if (Workspace* workspace = view->workspace()) {
            workspace->syncViewPresentation(view);
          }
        }
        ++restored;
      }
    }

    bool workspaceSelectionRestored = false;
    for (auto selection = m_displacedWorkspaceSelections.begin(); selection != m_displacedWorkspaceSelections.end();) {
      Output* output = outputFromName(selection->outputName);
      WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
      if (group == nullptr) {
        ++selection;
        continue;
      }

      Workspace* workspace = selection->workspaceNamed ? group->workspaceNamed(selection->workspaceName) : nullptr;
      if (!selection->workspaceNamed && group->dynamic()) {
        while (group->workspaceCount() <= selection->workspaceIndex) {
          if (group->insertDynamicWorkspace(group->workspaceCount()) == nullptr) {
            break;
          }
        }
        workspace = group->workspaceAt(selection->workspaceIndex);
        if (workspace != nullptr && (workspace->named() || workspace->displacedWorkspace())) {
          Workspace* inserted = group->insertDynamicWorkspace(selection->workspaceIndex);
          workspace = inserted != nullptr ? inserted : group->active();
        }
      }
      if (workspace == nullptr && group->workspaceCount() > 0) {
        workspace = group->workspaceAt(std::min(selection->workspaceIndex, group->workspaceCount() - 1));
      }
      if (workspace != nullptr && workspace->displacedWorkspace()) {
        workspace = nativeFallback(group);
      }
      if (workspace == nullptr) {
        ++selection;
        continue;
      }
      group->activate(workspace, false);
      selection = m_displacedWorkspaceSelections.erase(selection);
      workspaceSelectionRestored = true;
    }

    if (restored > 0) {
      kLog.info("restored {} displaced windows", restored);
      refreshSurfaceScales();
    }
    if (restored > 0 || workspaceSelectionRestored) {
      refocus();
    }

    // Refocus can reveal another scrolling lane. Reapply the snapshot anchor
    // synchronously while both the snapshot and its surviving views are alive.
    for (const RestoredViewport& restoredViewport : restoredViewports) {
      Workspace* workspace = restoredViewport.workspace;
      if (workspace == nullptr || workspace->layoutMode() != restoredViewport.snapshot->mode()) {
        continue;
      }
      if (ScrollingLayout* scrolling = workspace->scrollingLayout(); scrolling != nullptr) {
        scrolling->restoreSnapshotViewport(
            *restoredViewport.snapshot, workspace->scrollViewportExtent(), restoredViewport.geometryUnchanged
        );
      }
      workspace->markArrange(false);
    }
    if (restored > 0 || workspaceSelectionRestored) {
      scheduleIpcWindowsEvent();
    }
  }

  void Server::onOutputPowerSetMode(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_outputPowerSetMode);
    const auto* event = static_cast<wlr_output_power_v1_set_mode_event*>(data);
    for (const auto& output : self->m_outputs) {
      if (output->wlr() == event->output) {
        (void)output->setPowered(event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
        if (self->m_screenActivity != nullptr) {
          const bool anyOn =
              std::ranges::any_of(self->m_outputs, [](const auto& o) { return o->desktopEnabled() && !o->dpmsOff(); });
          self->m_screenActivity->setScreenOn(anyOn);
        }
        return;
      }
    }
  }

  void Server::onOutputManagerApply(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerApply);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), false);
  }

  void Server::onOutputManagerTest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerTest);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), true);
  }

  void Server::onOutputLayoutChange(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_outputLayoutChange);
    self->markDirty(Dirty::Backdrop);
    self->remapTouches();
    // A neighbour appearing, moving, or resizing changes where every output's content clip has to sit, and the clip is
    // refreshed from arrangeLayers.
    for (const auto& output : self->m_outputs) {
      output->markDirty(Dirty::LayerArrange);
    }
    if (!self->m_deferOutputManagerConfig) {
      self->updateOutputManagerConfig();
    }
    self->m_cursor->handleOutputLayoutChange();
    if (self->m_xwayland != nullptr) {
      self->m_xwayland->handleOutputLayoutChange();
    }
    if (self->m_eisInput != nullptr) {
      self->m_eisInput->updateOutputs();
    }
    if (self->m_inputCapture != nullptr) {
      self->m_inputCapture->updateOutputs();
    }
  }

  void Server::updateOutputManagerConfig() {
    // Every output state change that output management publishes also changes the IPC output listing.
    scheduleIpcOutputsEvent();
    if (m_inputCapture != nullptr && !m_deferOutputManagerConfig) {
      m_inputCapture->updateOutputs();
    }
    if (m_outputManager == nullptr || m_deferOutputManagerConfig) {
      return;
    }
    wlr_output_configuration_v1* cfg = wlr_output_configuration_v1_create();
    for (const auto& output : m_outputs) {
      wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(cfg, output->wlr());
      // Physical DPMS is not logical disablement. Protocol-disabled heads are
      // absent from the desktop; DPMS-off heads remain mapped there.
      head->state.enabled = output->desktopEnabled() || output->mirrorSource() != nullptr;
      if (wlr_output_layout_output* lo = wlr_output_layout_get(m_outputLayout, output->wlr())) {
        head->state.x = lo->x;
        head->state.y = lo->y;
      }
    }
    wlr_output_manager_v1_set_configuration(m_outputManager, cfg);
  }

  bool Server::initialOutputEnabled(const Output& output) {
    if (const std::optional<bool> override = m_outputEnableOverrides.resolve(output.identity())) {
      return *override;
    }
    return output.configuredEnabled();
  }

  void Server::rememberOutputEnableOverride(const Output& output, bool enabled) {
    m_outputEnableOverrides.remember(output.identity(), enabled);
  }

  bool Server::commitOutputEnabled(Output& target, bool enabled) {
    wlr_output_configuration_v1* config = wlr_output_configuration_v1_create();
    for (const auto& output : m_outputs) {
      wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(config, output->wlr());
      head->state.enabled = output.get() == &target ? enabled : output->desktopEnabled();
      const wlr_box box = output->layoutBox();
      head->state.x = box.x;
      head->state.y = box.y;
    }
    return applyOutputManagerConfig(config, false);
  }

  bool Server::setOutputEnabled(Output& output, bool enabled) {
    return output.desktopEnabled() == enabled || commitOutputEnabled(output, enabled);
  }

  bool Server::applyOutputManagerConfig(wlr_output_configuration_v1* config, bool testOnly) {
    size_t statesLen = 0;
    wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config, &statesLen);
    if (states == nullptr) {
      wlr_output_configuration_v1_send_failed(config);
      wlr_output_configuration_v1_destroy(config);
      return false;
    }

    struct RequestedHead {
      wlr_output_configuration_head_v1* head = nullptr;
      Output* output = nullptr;
      bool wasDesktopEnabled = false;
      bool wasMirror = false;
    };
    std::vector<RequestedHead> requested;
    wlr_output_configuration_head_v1* head = nullptr;
    wl_list_for_each(head, &config->heads, link) {
      if (Output* output = outputFromWlr(head->state.output)) {
        requested.push_back({
            .head = head,
            .output = output,
            .wasDesktopEnabled = output->desktopEnabled(),
            .wasMirror = output->mirrorSource() != nullptr,
        });
      }
    }

    // Output management describes logical desktop membership, while DPMS is
    // physical power. Keep an already sleeping logical output asleep. Its
    // advertised backend properties remain available for complete client
    // transactions, but reject changes that cannot be applied while asleep.
    bool sleepingStateValid = true;
    for (size_t i = 0; i < statesLen; ++i) {
      Output* output = outputFromWlr(states[i].output);
      if (output == nullptr || !output->desktopEnabled() || !output->dpmsOff() || !states[i].base.enabled) {
        continue;
      }
      if (!outputStateMatchesCurrentBackend(states[i].base, *states[i].output)) {
        kLog.warn("rejecting output-management property changes for DPMS-off output '{}'", states[i].output->name);
        sleepingStateValid = false;
        continue;
      }
      wlr_output_state_finish(&states[i].base);
      wlr_output_state_init(&states[i].base);
      wlr_output_state_set_enabled(&states[i].base, false);
    }

    struct SceneRectDestroy {
      void operator()(wlr_scene_rect* rect) const {
        if (rect != nullptr) {
          wlr_scene_node_destroy(&rect->node);
        }
      }
    };
    std::vector<std::unique_ptr<wlr_scene_rect, SceneRectDestroy>> configuredFrameMasks;
    const auto buildSceneStates = [this, &configuredFrameMasks](
                                      wlr_output_swapchain_manager& manager, wlr_backend_output_state* pending,
                                      size_t pendingLen
                                  ) {
      configuredFrameMasks.clear();
      for (size_t i = 0; i < pendingLen; ++i) {
        Output* output = outputFromWlr(pending[i].output);
        if (output == nullptr) {
          return false;
        }
        const bool enabled = (pending[i].base.committed & WLR_OUTPUT_STATE_ENABLED) != 0 ? pending[i].base.enabled
                                                                                         : pending[i].output->enabled;
        const bool unbound = wlr_output_layout_get(m_outputLayout, pending[i].output) == nullptr;
        if (enabled && (m_sessionLocked || unbound)) {
          wlr_scene_rect* mask = output->createConfiguredFrameMask(pending[i].base);
          if (mask == nullptr) {
            return false;
          }
          configuredFrameMasks.emplace_back(mask);
        }
        // This frame is built without the reveal pair, so resume native slide first.
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->abandonReveal();
        }
        // The pending mode, scale or transform may resize the optimized blur cache.
        output->markBlurBackgroundDirty();
        wlr_scene_output_state_options options{};
        options.swapchain = wlr_output_swapchain_manager_get_swapchain(&manager, pending[i].output);
        if (!wlr_scene_output_build_state(output->sceneOutput(), &pending[i].base, &options)) {
          kLog.error("failed to build output-management scene state for '{}'", pending[i].output->name);
          return false;
        }
      }
      return true;
    };

    const auto commitSceneStates = [this](wlr_backend_output_state* pending, size_t pendingLen) {
      std::vector<std::unique_ptr<Output::SceneCommit>> observations;
      observations.reserve(pendingLen);
      for (size_t i = 0; i < pendingLen; ++i) {
        if (Output* output = outputFromWlr(pending[i].output)) {
          observations.push_back(std::make_unique<Output::SceneCommit>(*output, pending[i].base, true));
        }
      }
      // Each output's successful commit event is authoritative even if a later child backend fails the group.
      return wlr_backend_commit(m_backend, pending, pendingLen);
    };

    struct BackendSnapshot {
      wlr_output* output = nullptr;
      bool enabled = false;
      wlr_output_mode* mode = nullptr;
      int width = 0;
      int height = 0;
      int refresh = 0;
      float scale = 1.0F;
      wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
      bool adaptiveSyncSupported = false;
      bool adaptiveSyncEnabled = false;
    };
    const auto stageSnapshot = [](wlr_output_state& state, const BackendSnapshot& snapshot, bool enabled) {
      wlr_output_state_set_enabled(&state, enabled);
      if (!enabled) {
        return;
      }
      if (snapshot.mode != nullptr) {
        wlr_output_state_set_mode(&state, snapshot.mode);
      } else {
        wlr_output_state_set_custom_mode(&state, snapshot.width, snapshot.height, snapshot.refresh);
      }
      wlr_output_state_set_scale(&state, snapshot.scale);
      wlr_output_state_set_transform(&state, snapshot.transform);
      if (snapshot.adaptiveSyncSupported) {
        wlr_output_state_set_adaptive_sync_enabled(&state, snapshot.adaptiveSyncEnabled);
      }
    };

    wlr_output_swapchain_manager swapchainManager{};
    wlr_output_swapchain_manager_init(&swapchainManager, m_backend);
    bool swapchainManagerFinished = false;
    bool ok = sleepingStateValid && wlr_output_swapchain_manager_prepare(&swapchainManager, states, statesLen);
    bool commitAttempted = false;
    std::vector<BackendSnapshot> snapshots;
    std::vector<wlr_backend_output_state> rollbackStates;
    if (ok && !testOnly) {
      snapshots.resize(statesLen);
      rollbackStates.resize(statesLen);
      for (size_t i = 0; i < statesLen; ++i) {
        wlr_output* output = states[i].output;
        BackendSnapshot& snapshot = snapshots[i];
        snapshot = {
            .output = output,
            .enabled = output->enabled,
            .mode = output->current_mode,
            .width = output->width,
            .height = output->height,
            .refresh = output->refresh,
            .scale = output->scale,
            .transform = output->transform,
            .adaptiveSyncSupported = output->adaptive_sync_supported,
            .adaptiveSyncEnabled = output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED,
        };
        wlr_backend_output_state& rollback = rollbackStates[i];
        rollback.output = output;
        wlr_output_state_init(&rollback.base);
        stageSnapshot(rollback.base, snapshot, snapshot.enabled);
      }

      ok = buildSceneStates(swapchainManager, states, statesLen);
      if (ok) {
        // Commits can synchronously emit layout changes for mode, scale, and
        // transform updates. Suppress those partial configurations until the
        // logical layout transaction is complete.
        m_deferOutputManagerConfig = true;
        commitAttempted = true;
        ok = commitSceneStates(states, statesLen);
        if (ok) {
          wlr_output_swapchain_manager_apply(&swapchainManager);
        }
      }

      if (!ok && commitAttempted) {
        wlr_output_swapchain_manager_finish(&swapchainManager);
        swapchainManagerFinished = true;
        for (size_t i = 0; i < statesLen; ++i) {
          wlr_output_state_finish(&states[i].base);
          wlr_output_state_init(&states[i].base);
        }

        wlr_output_swapchain_manager rollbackManager{};
        wlr_output_swapchain_manager_init(&rollbackManager, m_backend);
        bool restoredAsGroup =
            wlr_output_swapchain_manager_prepare(&rollbackManager, rollbackStates.data(), rollbackStates.size());
        if (restoredAsGroup) {
          restoredAsGroup = buildSceneStates(rollbackManager, rollbackStates.data(), rollbackStates.size())
              && commitSceneStates(rollbackStates.data(), rollbackStates.size());
        }
        if (restoredAsGroup) {
          wlr_output_swapchain_manager_apply(&rollbackManager);
        }
        wlr_output_swapchain_manager_finish(&rollbackManager);

        // Release any buffers built from the grouped rollback swapchains
        // before trying a one-output recovery path.
        for (wlr_backend_output_state& rollback : rollbackStates) {
          wlr_output_state_finish(&rollback.base);
        }
        rollbackStates.clear();

        const auto restoreEnabled = [this, &buildSceneStates, &commitSceneStates,
                                     &stageSnapshot](const BackendSnapshot& snapshot) {
          wlr_backend_output_state restore{};
          restore.output = snapshot.output;
          wlr_output_state_init(&restore.base);
          stageSnapshot(restore.base, snapshot, true);

          wlr_output_swapchain_manager manager{};
          wlr_output_swapchain_manager_init(&manager, m_backend);
          bool restored = wlr_output_swapchain_manager_prepare(&manager, &restore, 1);
          if (restored) {
            restored = buildSceneStates(manager, &restore, 1) && commitSceneStates(&restore, 1);
          }
          if (restored) {
            wlr_output_swapchain_manager_apply(&manager);
          }
          wlr_output_swapchain_manager_finish(&manager);
          wlr_output_state_finish(&restore.base);
          return restored;
        };

        // A successful child-backend commit can change the remembered mode of
        // an output which was disabled before the transaction. Restore each
        // such head alone and disable it immediately, avoiding resource
        // pressure from temporarily enabling several dormant heads at once.
        const auto restoreDormant = [&restoreEnabled, &stageSnapshot](const BackendSnapshot& snapshot) {
          wlr_output_state expected{};
          wlr_output_state_init(&expected);
          stageSnapshot(expected, snapshot, true);
          bool restored = (snapshot.mode == nullptr && (snapshot.width <= 0 || snapshot.height <= 0))
              || outputStateMatchesCurrentBackend(expected, *snapshot.output)
              || restoreEnabled(snapshot);
          wlr_output_state_finish(&expected);

          if (snapshot.output->enabled) {
            wlr_output_state disabled{};
            wlr_output_state_init(&disabled);
            wlr_output_state_set_enabled(&disabled, false);
            restored = wlr_output_commit_state(snapshot.output, &disabled) && restored;
            wlr_output_state_finish(&disabled);
          }
          return restored;
        };

        bool restoredIndividually = restoredAsGroup;
        if (restoredAsGroup) {
          for (const BackendSnapshot& snapshot : snapshots) {
            if (!snapshot.enabled && !restoreDormant(snapshot)) {
              restoredIndividually = false;
            }
          }
        } else {
          restoredIndividually = true;
          for (const BackendSnapshot& snapshot : snapshots) {
            if (!snapshot.enabled && !restoreDormant(snapshot)) {
              restoredIndividually = false;
            }
          }
          for (const BackendSnapshot& snapshot : snapshots) {
            if (snapshot.enabled && !restoreEnabled(snapshot)) {
              restoredIndividually = false;
            }
          }
        }

        if (restoredIndividually) {
          if (!restoredAsGroup) {
            kLog.warn("restored a partial output-management commit one output at a time");
          }
        } else {
          kLog.error("failed to restore all outputs after a partial output-management commit");
        }
      }
    }

    if (ok && !testOnly) {
      // Make logical enablement authoritative before any callback can refresh
      // configured output policy and accidentally revive a disabled head.
      for (const RequestedHead& entry : requested) {
        rememberOutputEnableOverride(*entry.output, entry.head->state.enabled);
        // Mirrors are reported enabled: enabled keeps the mirror with its new mode, disabled ends it and powers off.
        if (entry.output->mirrorSource() != nullptr) {
          if (entry.head->state.enabled) {
            // The commit above drew the mirror's own off-layout scene: redraw the source's frame into it.
            entry.output->scheduleFullFrame();
            continue;
          }
          entry.output->setMirrorSource(nullptr, /*applyState=*/false);
          if (const std::string name = savedOutputName(entry.output->identity()); !documentSetsOutput(configRootPath(), name)) {
            (void)saveMirror(configRootPath().parent_path() / "displays.toml", name, std::nullopt);
          }
        }
        entry.output->adoptOutputManagerEnabled(entry.head->state.enabled);
      }

      cancelModifierTap();
      m_cursor->cancelLayoutInteraction();
      m_gestures->cancelForLayoutChange();
      m_overview->forceClose();
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->slideFinish();
        }
      }

      // Layout mutations emit synchronously. Add every destination before
      // removing sources, then publish only the finished transaction.
      for (const RequestedHead& entry : requested) {
        if (entry.head->state.enabled && entry.output->mirrorSource() == nullptr) {
          entry.output->applyOutputManagerLayout(entry.head->state.x, entry.head->state.y);
        }
      }
      for (const RequestedHead& entry : requested) {
        if (!entry.head->state.enabled) {
          entry.output->applyOutputManagerLayout(entry.head->state.x, entry.head->state.y);
        }
      }

      for (const RequestedHead& entry : requested) {
        if (!entry.wasDesktopEnabled || entry.head->state.enabled) {
          continue;
        }
        Output* fallback = nullptr;
        for (const auto& candidate : m_outputs) {
          if (candidate.get() != entry.output && candidate->desktopEnabled() && candidate->wlr()->enabled) {
            fallback = candidate.get();
            break;
          }
        }
        reassignOutputViews(entry.output, fallback);
      }
      scheduleDisplacedViewRestore();
      markDirty(Dirty::Banner | Dirty::QuitConfirm);
      if (m_sessionLocked) {
        updateLockBlank();
      }
      updateIdleInhibit();
      updateColorPreferences();
      refocus();
      refreshSurfaceScales();
      std::vector<SavedOutput> saved;
      saved.reserve(requested.size());
      for (const RequestedHead& entry : requested) {
        const wlr_output_head_v1_state& state = entry.head->state;
        std::string name = savedOutputName(entry.output->identity());
        if (documentSetsOutput(configRootPath(), name)) {
          kLog.warn(
              "output '{}' set in {}, which overrides changes made here; not persisting to displays.toml", name,
              configRootPath().filename().string()
          );
          continue;
        }
        saved.push_back({
            .name = std::move(name),
            .enabled = state.enabled,
            .width = state.mode != nullptr ? state.mode->width : state.custom_mode.width,
            .height = state.mode != nullptr ? state.mode->height : state.custom_mode.height,
            .refreshMHz = state.mode != nullptr ? state.mode->refresh : state.custom_mode.refresh,
            .position = entry.wasMirror ? std::nullopt : std::optional(std::array{state.x, state.y}),
            .scale = state.scale,
            .transform = static_cast<int>(state.transform),
            .adaptiveSync = state.adaptive_sync_enabled,
        });
      }
      if (const auto file = configRootPath().parent_path() / "displays.toml"; !saveOutputs(file, saved)) {
        kLog.warn("failed to save output state to '{}'", file.string());
      }
    }
    if (commitAttempted) {
      m_deferOutputManagerConfig = false;
    }

    if (!swapchainManagerFinished) {
      wlr_output_swapchain_manager_finish(&swapchainManager);
    }
    for (size_t i = 0; i < statesLen; ++i) {
      wlr_output_state_finish(&states[i].base);
    }
    for (wlr_backend_output_state& rollback : rollbackStates) {
      wlr_output_state_finish(&rollback.base);
    }
    free(states);
    if (ok) {
      wlr_output_configuration_v1_send_succeeded(config);
    } else {
      wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    if (commitAttempted) {
      updateOutputManagerConfig();
    }
    return ok;
  }

} // namespace andromeda
