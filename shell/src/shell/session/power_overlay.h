#pragma once

#include "render/animation/animation_manager.h"
#include "compositors/wayland_toplevels.h"
#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/session/session_end_phase.h"
#include "wayland/layer_surface.h"

#include <sdbus-c++/sdbus-c++.h>

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CountdownRingNode;
class Flex;
class CompositorPlatform;
class ConfigService;
class Glyph;
class InputArea;
class IpcService;
class Label;
class Node;
class PowerConfigService;
class SessionActionRunner;
class SessionGuard;
class InhibitPortal;
class SystemBus;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

// D3 power choices on the blurred desktop (.agent/DESIGN.md "Power menu and session end", mockup power-menu.html
// section 1): Lock / Sleep / Restart / Shut Down as 88 px round buttons, the focused one white. Arrows move, Enter
// picks, Esc or a click on the desktop closes. Holding the power key fills a ring for 2 s, then shuts down after a
// 0.5 s fade; a release cancels. Opens on Ctrl+Alt+Delete and a power-key tap (`noctalia msg power-overlay`).
//
// Restart, Shut Down and Log Out end the session in place (sections 2 and 3): every window is asked to close; when all
// close, an alert counts 60 s (paused while the pointer is on it) and then acts; windows still open after 5 s are
// listed with Show and Force Quit, and the action goes on by itself once the list is empty.
class PowerOverlay {
public:
  void initialize(
      WaylandConnection& wayland, RenderContext* renderContext, ConfigService* config, IpcService* ipc,
      CompositorPlatform* platform
  );
  void registerIpc(IpcService& ipc);
  // Takes logind's handle-power-key block, so the key comes to us (as gnome-settings-daemon does) and follows the
  // Settings › Power choice: the power menu (logind "poweroff"), Sleep, Hibernate, Lock or nothing.
  void setPowerKeyHandling(SystemBus* bus, const PowerConfigService* power, std::function<bool()> locked);
  // Other users' sessions show in the session-end alert; the guard is watched while a session end runs.
  void setSessionGuard(SessionGuard* guard) { m_guard = guard; }
  // Apps that asked the Inhibit portal to delay logout join the blocker list with their reason.
  void setInhibitPortal(InhibitPortal* portal) { m_portal = portal; }
  void onGuardChanged();
  // Runs the session-end action and reports its failure back, so the portal leaves Ending only for that run.
  void setSessionActionRunner(SessionActionRunner* runner) { m_runner = runner; }
  // "Reopen windows when you log back in" (D3): the apps open when the last session ended with it checked. Returns
  // them once, then forgets them.
  [[nodiscard]] static std::vector<std::string> takeReopenApps();
  void show(wl_output* output);
  void hide();
  // `action` is reboot, shutdown or logout.
  void beginEnd(const std::string& action);
  void onOutputChange();
  void onToplevelChange();
  [[nodiscard]] bool isActive() const noexcept { return m_surface != nullptr; }
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

private:
  struct Choice {
    InputArea* area = nullptr;
    Flex* circle = nullptr;
    Glyph* glyph = nullptr;
    Label* label = nullptr;
  };

  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(float width, float height);
  void syncFocus();
  void pick(std::size_t index);
  void startHold();
  void cancelHold();
  void run(const char* action);
  void powerKeyDown();
  void powerKeyUp();
  enum class Mode : std::uint8_t { Choices, Closing, Countdown, Blockers, Ending };
  void setMode(Mode mode);
  // Drops the surface but keeps the session-end state, so windows behind it take input.
  void hideSurface();
  // Countdown and Blockers survive a lost surface: the sheet comes back, or the action goes on without an output.
  void onSurfaceLost();
  void restoreSheet();
  void startCountdown();
  void buildChoices(Node& root, float scale);
  void buildCountdown(Node& root, float scale);
  void buildBlockers(Node& root, float scale);
  [[nodiscard]] std::vector<ToplevelInfo> openWindows() const;
  void checkWindows();
  void tickCountdown();
  void finishEnd();
  void runEnd(const std::string& action, std::uint64_t generation);
  [[nodiscard]] std::string endText(const char* kind) const;

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  ConfigService* m_config = nullptr;
  IpcService* m_ipc = nullptr;
  SessionActionRunner* m_runner = nullptr;
  session_end::Machine m_sessionEnd;
  CompositorPlatform* m_platform = nullptr;
  Mode m_mode = Mode::Choices;
  std::string m_endAction;
  Timer m_closeTimer;
  Timer m_tickTimer;
  int m_secondsLeft = 0;
  bool m_countPaused = false;
  Label* m_countLabel = nullptr;
  Node* m_level = nullptr;
  std::vector<ToplevelInfo> m_blockers;
  wl_output* m_output = nullptr;
  std::unique_ptr<LayerSurface> m_surface;
  AnimationManager m_animations;
  InputDispatcher m_input;
  std::unique_ptr<Node> m_root;
  InputArea* m_backdrop = nullptr;
  Node* m_content = nullptr;
  Label* m_hint = nullptr;
  CountdownRingNode* m_ring = nullptr;
  std::array<Choice, 4> m_choices{};
  std::size_t m_focused = 0;
  bool m_pointerInside = false;
  bool m_holding = false;
  float m_holdProgress = 0.0F;
  AnimationManager::Id m_holdAnim = 0;
  std::unique_ptr<sdbus::IProxy> m_logind;
  sdbus::UnixFd m_powerKeyInhibit;
  const PowerConfigService* m_power = nullptr;
  std::function<bool()> m_locked;
  // The choices were already open when this power-key press began: a tap closes them.
  bool m_openBeforePress = false;
  SessionGuard* m_guard = nullptr;
  InhibitPortal* m_portal = nullptr;
  // Logout inhibitors as of the last check: app id, reason.
  std::vector<std::pair<std::string, std::string>> m_appBlockers;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> appBlockers() const;
  // Sets the portal from the mode and the session-end machine; called after every change of either.
  void syncPortal();
  bool m_guardWatched = false;
  bool m_reopen = true;
  std::vector<std::string> m_reopenApps;
  void saveReopen(const std::vector<std::string>& apps) const;
  void readUnresponsive();
  // Window ids the compositor reports as not answering its close ping; read when the blocker list changes.
  std::vector<std::string> m_unresponsive;
  void watchGuard(bool on);
};
