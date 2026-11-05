#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/toplevel_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class AppLoop;
class Button;
class CommandTable;
class Flex;
class Label;
class Renderer;
class StateStore;
struct KeyboardEvent;
struct PointerEvent;

// A first-party app window (docs/apps.md §2, rulebook §9.12): the 48 px app header with back / forward, title and
// window buttons, over the app's content. Size and maximized state persist in the app's StateStore under
// /window/<stateKey>/.
class AppWindow {
public:
  struct Config {
    std::string appId;
    std::string title;
    std::string stateKey = "main";
    std::uint32_t width = 960; // logical px before ui scale, used when no state is saved
    std::uint32_t height = 640;
    std::uint32_t minWidth = 480;
    std::uint32_t minHeight = 320;
    bool navigation = false; // back / forward buttons in the header (commands go.back, go.forward)
  };

  // Builds the content under the header; called on open, on requestRebuild() and when the scale changes.
  using ContentBuilder = std::function<std::unique_ptr<Node>(Renderer& renderer, float width, float height)>;

  AppWindow(AppLoop& loop, CommandTable& commands, StateStore& state, Config config);
  ~AppWindow();

  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  bool open();
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }

  void setContentBuilder(ContentBuilder builder) { m_builder = std::move(builder); }
  void setClosedCallback(std::function<void()> callback) { m_onClosed = std::move(callback); }
  void setTitle(std::string title);
  void requestRebuild();
  // Repaints or relayouts after a node changed in place.
  void invalidate();
  // Raises the window for a launch that carried an xdg-activation token.
  void activate(const std::string& token);
  void toggleMaximized();

  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] float uiScale() const noexcept { return m_uiScale; }
  [[nodiscard]] InputDispatcher& input() noexcept { return m_input; }
  [[nodiscard]] ToplevelSurface* surface() noexcept { return m_surface.get(); }

  // The app routes events here only for this window's surface (AppWindows::route*).
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

private:
  void prepareFrame(bool needsLayout);
  void buildScene(Renderer& renderer, float width, float height);
  [[nodiscard]] std::unique_ptr<Flex> buildHeader();
  void saveGeometry();
  // xdg_toplevel resize edges under the pointer, 0 outside the band or while maximized.
  [[nodiscard]] std::uint32_t resizeEdgesAt(float x, float y) const;
  void onClosedByCompositor();
  [[nodiscard]] std::string key(const char* field) const;

  AppLoop& m_loop;
  CommandTable& m_commands;
  StateStore& m_state;
  Config m_config;
  ContentBuilder m_builder;
  std::function<void()> m_onClosed;
  float m_uiScale = 1.0F;

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Node> m_root;
  Label* m_titleLabel = nullptr;
  Button* m_maximizeButton = nullptr;
  AnimationManager m_animations;
  InputDispatcher m_input;
  bool m_rebuild = true;
  bool m_buttonDown = false;
  bool m_pointerInside = false;
  bool m_inResizeBand = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // deferred lambdas skip a destroyed window
};
