#pragma once

#include "shell/bar/widget.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ContextMenuControlEntry;
class Box;
class DbusMenuClient;
class GtkMenuClient;
struct DbusMenuNode;
class SessionBus;
class CompositorPlatform;
class ConfigService;
class ContextMenuPopup;
class InputArea;
class Label;
class RenderContext;
struct wl_output;

// macOS layout top bar (board desktop-menus.html A1-A3): the focused app's name in bold, then the app's own menus
// (com.canonical.dbusmenu or org.gtk.Menus) or, when it exports none, a Window menu the compositor owns (Minimize, Zoom, Fullscreen,
// Move to, Close, Quit). The name opens the Window menu. Items are 24 high, r 8: hover fill_1, open fill_2.
class WindowMenuWidget : public Widget {
public:
  WindowMenuWidget(
      wl_output* output, ConfigService& config, CompositorPlatform& platform, RenderContext& renderContext,
      std::string barPosition, SessionBus* bus
  );
  ~WindowMenuWidget() override;

  void create() override;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event) override;
  // Each item draws its own hover and open fill; no capsule behind the whole menu row.
  [[nodiscard]] bool wantsBarHoverHighlight() const noexcept override { return false; }

private:
  struct Item {
    InputArea* area = nullptr;
    Box* fill = nullptr;
    Label* label = nullptr;
    bool hovered = false;
    // Index of the app menu's top-level node; -1 opens the Window menu (the app name, and the fallback "Window"),
    // -2 is the » item.
    std::int32_t menuId = -1;
    // In the » menu, or the » item while everything fits.
    bool hidden = false;
  };
  [[nodiscard]] const DbusMenuNode* appMenuRoot() const;
  void rebuildItems();
  void openAppMenu(std::size_t item);
  void openPopup(std::size_t index, std::vector<ContextMenuControlEntry> entries);
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncFills();
  void openMenu(std::size_t item);

  wl_output* m_output = nullptr;
  ConfigService& m_config;
  CompositorPlatform& m_platform;
  RenderContext& m_renderContext;
  std::string m_barPosition;
  SessionBus* m_bus = nullptr;
  std::vector<Item> m_items;
  // Shared so popup callbacks hold them weakly: a deferred activation never reaches a newer app's client.
  std::shared_ptr<DbusMenuClient> m_appMenu;
  std::shared_ptr<GtkMenuClient> m_gtkMenu;
  bool m_itemsDirty = true;
  std::unique_ptr<ContextMenuPopup> m_menu;
  std::size_t m_openItem = static_cast<std::size_t>(-1);
  float m_barWidth = 0.0F;
  bool m_switchingMenu = false;
  std::string m_appId;
  std::string m_appName;
  std::string m_windowId;
  std::string m_menuKey; // kind, service, menu path, application path, window path
};
