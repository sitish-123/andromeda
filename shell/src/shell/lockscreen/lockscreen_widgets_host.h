#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "shell/lockscreen/lock_helper_process.h"
#include "shell/desktop/desktop_widget_factory.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class LockScreen;
class LockSurface;
class PixelStreamView;
namespace scripting {
  class ScriptApiContext;
}
class RenderContext;
class WaylandConnection;

using LockscreenWidgetsSnapshot = LockscreenWidgetsConfig;

class LockscreenWidgetsHost {
public:
  LockscreenWidgetsHost() = default;
  // Stream callbacks capture the host: stop the helper and drop the views first.
  ~LockscreenWidgetsHost() { hide(); }
  LockscreenWidgetsHost(const LockscreenWidgetsHost&) = delete;
  LockscreenWidgetsHost& operator=(const LockscreenWidgetsHost&) = delete;

  void initialize(const DesktopWidgetServices& services);
  void show(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen);
  void hide();
  void rebuild(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen);
  // Destroy and recreate every plugin-backed instance so its Luau runtime is re-seeded.
  // Needed because plugin-level settings live outside the widget snapshot, so syncSurfaces()
  // cannot see them change.
  void reloadPluginWidgets(LockScreen& lockScreen);
  void onOutputChange(LockScreen& lockScreen);
  void onSecondTick();
  // True when a live widget renders a value that changes every second.
  [[nodiscard]] bool wantsSecondTicks() const;
  void prepareFrame(LockSurface& surface, bool needsUpdate, bool needsLayout);

private:
  struct WidgetInstance {
    DesktopWidgetState state;
    LockSurface* surface = nullptr;
    AnimationManager animations;
    Node* transformNode = nullptr;
    std::unique_ptr<DesktopWidget> widget;
    // Plugin widgets draw in the lock helper process; their frames arrive in `stream`.
    PixelStreamView* stream = nullptr;
    Node* placeholder = nullptr;
    // Output scale the helper renders this slot at; a change needs a new helper.
    float streamScale = 0.0F;
    float intrinsicWidth = 0.0F;
    float intrinsicHeight = 0.0F;
  };

  void syncSurfaces(LockScreen& lockScreen);
  void createInstance(const DesktopWidgetState& state, LockSurface& surface, const WaylandOutput& output);
  void createStreamInstance(const DesktopWidgetState& state, LockSurface& surface);
  void scheduleHelper(std::chrono::milliseconds delay);
  void startHelper();
  void onStreamDropped();
  void attachToSurface(WidgetInstance& instance);
  void detachFromSurface(WidgetInstance& instance);
  void syncSurfaceFrameTick(LockSurface* surface);
  [[nodiscard]] WidgetInstance* findInstance(const std::string& id);
  [[nodiscard]] LockSurface* findSurfaceForOutput(LockScreen& lockScreen, const std::string& outputKey) const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  LockscreenWidgetsSnapshot m_snapshot;
  bool m_visible = false;
  std::vector<std::unique_ptr<WidgetInstance>> m_instances;
  scripting::ScriptApiContext* m_scriptApi = nullptr;
  LockHelperProcess m_helper;
  Timer m_helperTimer;
  int m_helperRestarts = 0;
};
