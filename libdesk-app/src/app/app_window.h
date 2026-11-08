#pragma once

#include "ui/controls/context_menu.h"

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/toplevel_surface.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class AppLoop;
class Button;
class ContextMenuPopup;
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
namespace desk {
  class ForeignParent;
}

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
    float sidebarWidth = 0.0F; // logical px before ui scale; 0 = no sidebar
    std::string parentWindow;  // portal form "wayland:<xdg-foreign handle>": a dialog of another app's window
    bool transparentContent = false; // the content area shows what lies below the window surface (a video subsurface)
    bool headerWhenFullscreen = true;
    bool header = true; // false: a chromeless window (picture in picture); the content is the whole window
    bool moreMenu = false; // a More (…) circle before the window buttons: the commands by group (File, Edit, View, Go)
  };

  // Builds the content under the header; called on open, on requestRebuild() and when the scale changes.
  using ContentBuilder = std::function<std::unique_ptr<Node>(Renderer& renderer, float width, float height)>;
  // The full-height sidebar left of the header and content (rulebook §9.12, Settings-style opaque sidebar).
  using SidebarBuilder = std::function<std::unique_ptr<Node>(Renderer& renderer, float width, float height)>;

  AppWindow(AppLoop& loop, CommandTable& commands, StateStore& state, Config config);
  ~AppWindow();

  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  bool open();
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }

  void setContentBuilder(ContentBuilder builder) { m_builder = std::move(builder); }
  void setSidebarBuilder(SidebarBuilder builder) { m_sidebarBuilder = std::move(builder); }
  // App controls in the header between the title and the window buttons (rulebook 9.12: view switch, search).
  void setHeaderAccessoryBuilder(std::function<std::unique_ptr<Node>()> builder) {
    m_headerAccessory = std::move(builder);
  }
  [[nodiscard]] AnimationManager& animations() noexcept { return m_animations; }
  // Keys no command took and no text field owns (type-ahead, arrows in a list). True = handled.
  void setKeyHandler(std::function<bool(const KeyboardEvent&)> handler) { m_keyHandler = std::move(handler); }
  // A window-modal sheet (rulebook 9.10): a scrim over the window and the card centred on it. The builder runs on
  // each scene build; Escape calls onCancel. Window keys and commands wait while it shows.
  void presentSheet(std::function<std::unique_ptr<Node>(float scale)> builder, std::function<void()> onCancel);
  void dismissSheet();
  // Collapses or restores the sidebar on spring.page (the Files Gallery is full-bleed); the header stays in place.
  void setSidebarVisible(bool visible);
  // Asks for a new window size (logical px before ui scale). False when the compositor sizes the window: then the
  // size becomes the minimum, so the compositor grows it.
  bool requestSize(std::uint32_t width, std::uint32_t height);
  // Saves the current geometry under the old key, then remembers size and state under this one.
  void setStateKey(std::string stateKey);
  [[nodiscard]] bool hasSheet() const noexcept { return static_cast<bool>(m_sheetBuilder); }
  void setClosedCallback(std::function<void()> callback) { m_onClosed = std::move(callback); }
  // Runs when the compositor activates or deactivates the window (xdg_toplevel activated state).
  void setActiveCallback(std::function<void(bool active)> callback) { m_onActive = std::move(callback); }
  void setTitle(std::string title);
  void requestRebuild();
  // Updates the back / forward buttons from their commands without a rebuild.
  void refreshNavigation();
  // Repaints or relayouts after a node changed in place.
  void invalidate();
  // Raises the window for a launch that carried an xdg-activation token.
  void activate(const std::string& token);
  // A context menu at a point in window (scene) coordinates; `activated` gets the entry id, after the menu closed.
  void openContextMenu(std::vector<ContextMenuControlEntry> entries, float sceneX, float sceneY,
                       std::function<void(std::int32_t id)> activated);
  // A popover under `anchor` (a node of this window), `width` x `height` logical px before ui scale.
  void openPopover(const Node* anchor, float width, float height, std::function<std::unique_ptr<Node>(float scale)> content);
  void refreshPopover();
  void closePopover();
  [[nodiscard]] bool popoverOpen() const noexcept;
  void toggleMaximized();
  void toggleFullscreen();
  [[nodiscard]] bool fullscreen() const noexcept;
  // The content area in surface coordinates (logical px), as of the last layout.
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };
  [[nodiscard]] Rect contentRect() const noexcept { return m_contentRect; }
  // After each layout (size, scale or fullscreen change).
  void setLayoutCallback(std::function<void()> callback) { m_onLayout = std::move(callback); }

  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  // This window's surface or its open menu popup: the app routes that surface's events here.
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
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
  void openMoreMenu();

  AppLoop& m_loop;
  CommandTable& m_commands;
  StateStore& m_state;
  Config m_config;
  ContentBuilder m_builder;
  SidebarBuilder m_sidebarBuilder;
  std::function<std::unique_ptr<Node>()> m_headerAccessory;
  std::function<bool(const KeyboardEvent&)> m_keyHandler;
  std::function<void(bool)> m_onActive;
  std::function<std::unique_ptr<Node>(float)> m_sheetBuilder;
  std::function<void()> m_sheetCancel;
  Node* m_sheetCard = nullptr;
  float m_sheetMix = 1.0F; // open motion: card opacity, scale 0.96 -> 1 (M31)
  std::uint32_t m_sheetAnim = 0;
  std::function<void()> m_onClosed;
  std::function<void()> m_onLayout;
  Rect m_contentRect;
  float m_uiScale = 1.0F;

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<desk::ForeignParent> m_foreignParent;
  std::unique_ptr<Node> m_root;
  Label* m_titleLabel = nullptr;
  Button* m_maximizeButton = nullptr;
  std::array<Button*, 2> m_navButtons{};
  Button* m_moreButton = nullptr;
  Flex* m_sidebarNode = nullptr;
  float m_sidebarMix = 1.0F; // 0 = collapsed
  bool m_sidebarVisible = true;
  std::uint32_t m_sidebarAnim = 0;
  std::unique_ptr<ContextMenuPopup> m_menu;
  std::unique_ptr<class AppPopover> m_popover;
  AnimationManager m_animations;
  InputDispatcher m_input;
  bool m_rebuild = true;
  bool m_buttonDown = false;
  bool m_pointerInside = false;
  bool m_inResizeBand = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // deferred lambdas skip a destroyed window
};
