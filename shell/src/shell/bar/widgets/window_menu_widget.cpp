#include "shell/bar/widgets/window_menu_widget.h"

#include "shell/bar/widgets/dbus_menu_client.h"
#include "shell/bar/widgets/gtk_menu_client.h"

#include "compositors/andromeda/andromeda_runtime.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "xdg-shell-client-protocol.h"

#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <vector>

namespace {
  // Board A1: items 24 high with 8 side padding, r 8; the app name ellipsizes at 200. Board A2: menus 220 wide.
  constexpr int kItemHeight = 24;
  constexpr int kItemPad = 8;
  constexpr int kItemRadius = 8;
  constexpr int kNameMax = 200;
  constexpr int kMenuWidth = 220;
  // Half the 160 center-island rest pill plus a gap.
  constexpr int kCenterReserve = 96;
  constexpr std::int32_t kOverflow = -2;

  // A menu row, then its submenu's rows one indent deeper (one level; no nested popups).
  void addMenuRows(std::vector<ContextMenuControlEntry>& entries, const DbusMenuNode& node, std::uint8_t indent) {
    if (!node.visible) {
      return;
    }
    if (node.separator) {
      entries.push_back({.separator = true});
      return;
    }
    entries.push_back({
        .id = node.id,
        .label = node.label,
        .shortcut = node.shortcut,
        .enabled = node.enabled,
        .header = !node.children.empty(),
        .checkmark = node.checkmark,
        .radio = node.radio,
        .toggleState = node.toggleState,
        .indentLevel = indent,
    });
    for (const DbusMenuNode& child : node.children) {
      if (!child.visible) {
        continue;
      }
      if (child.separator) {
        entries.push_back({.separator = true});
        continue;
      }
      entries.push_back({
          .id = child.id,
          .label = child.label,
          .shortcut = child.shortcut,
          .enabled = child.enabled,
          .header = !child.children.empty(),
          .checkmark = child.checkmark,
          .radio = child.radio,
          .toggleState = child.toggleState,
          .indentLevel = static_cast<std::uint8_t>(indent + 1),
      });
    }
  }

  enum Action : std::int32_t {
    Minimize = 1,
    Zoom,
    Fullscreen,
    Close,
    Quit,
    // Workspace rows use kWorkspaceBase + index.
    kWorkspaceBase = 100,
  };
} // namespace

WindowMenuWidget::WindowMenuWidget(
    wl_output* output, ConfigService& config, CompositorPlatform& platform, RenderContext& renderContext,
    std::string barPosition, SessionBus* bus
)
    : m_output(output), m_config(config), m_platform(platform), m_renderContext(renderContext),
      m_barPosition(std::move(barPosition)), m_bus(bus) {}

WindowMenuWidget::~WindowMenuWidget() {
  // The popup closes in its destructor; its dismiss callback must not reach a bar that is being torn down.
  if (m_menu != nullptr) {
    m_menu->setOnDismissed({});
  }
}

void WindowMenuWidget::create() { setRoot(ui::inputArea({})); }

const DbusMenuNode* WindowMenuWidget::appMenuRoot() const {
  if (m_appMenu != nullptr && m_appMenu->loaded()) {
    return &m_appMenu->root();
  }
  if (m_gtkMenu != nullptr && m_gtkMenu->loaded()) {
    return &m_gtkMenu->root();
  }
  return nullptr;
}

// Items: the bold app name, then the app's own menus, or the Window menu when it exports none (board A1, A3).
void WindowMenuWidget::rebuildItems() {
  Node* rootNode = root();
  if (rootNode == nullptr) {
    return;
  }
  if (m_menu != nullptr && m_menu->isOpen()) {
    m_menu->close();
  }
  while (!rootNode->children().empty()) {
    (void)rootNode->removeChild(rootNode->children().back().get());
  }
  std::vector<std::pair<std::string, std::int32_t>> labels{{m_appName, -1}};
  if (const DbusMenuNode* menuRoot = appMenuRoot(); menuRoot != nullptr) {
    for (std::size_t i = 0; i < menuRoot->children.size(); ++i) {
      const DbusMenuNode& top = menuRoot->children[i];
      if (top.visible && !top.separator && !top.label.empty()) {
        labels.emplace_back(top.label, static_cast<std::int32_t>(i));
      }
    }
  }
  if (labels.size() == 1) {
    labels.emplace_back(i18n::tr("bar.window-menu.window"), -1);
  } else {
    // Shown by doLayout only when the menus do not fit (board A4).
    labels.emplace_back("»", kOverflow);
  }
  m_items.assign(labels.size(), Item{});
  for (std::size_t i = 0; i < labels.size(); ++i) {
    Item& item = m_items[i];
    item.menuId = labels[i].second;
    auto area = ui::inputArea({
        .out = &item.area,
        .acceptedButtons = InputArea::buttonMask({BTN_LEFT}),
        .participatesInLayout = false,
        .onEnter =
            [this, i](const InputArea::PointerData&) {
              m_items[i].hovered = true;
              // An open menu follows the pointer to the next item, as on macOS.
              if (m_menu != nullptr && m_menu->isOpen() && m_openItem != i) {
                openMenu(i);
              }
              syncFills();
            },
        .onLeave =
            [this, i] {
              m_items[i].hovered = false;
              syncFills();
            },
        .onPress =
            [this, i](const InputArea::PointerData& data) {
              if (data.pressed && data.button == BTN_LEFT) {
                openMenu(i);
              }
            },
    });
    area->addChild(ui::box({.out = &item.fill, .radius = kItemRadius * m_contentScale, .visible = false}));
    area->addChild(ui::label({
        .out = &item.label,
        .text = labels[i].first,
        .fontSize = Style::fontSizeBody * m_contentScale,
        .fontWeight = i == 0 ? FontWeight::Bold : FontWeight::Normal,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxWidth = kNameMax * m_contentScale,
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    rootNode->addChild(std::move(area));
  }
  m_itemsDirty = false;
}

void WindowMenuWidget::doUpdate(Renderer& renderer) {
  const auto current = m_platform.activeToplevel();
  std::string appId = current.has_value() ? current->appId : std::string{};
  // An open menu takes keyboard focus from the app, so the app reads as unfocused until it closes: only a switch to
  // another app closes it.
  if (m_menu != nullptr && m_menu->isOpen() && (appId.empty() || appId == m_appId)) {
    return;
  }
  const auto address = m_platform.focusedWindowAppMenu();
  const bool hasMenu = address.has_value() && m_bus != nullptr && !address->service.empty() && !address->path.empty();
  const bool gtk = hasMenu && address->gtk;
  std::string menuKey = hasMenu
      ? std::string(gtk ? "gtk\n" : "dbusmenu\n") + address->service + "\n" + address->path + "\n"
            + address->applicationPath + "\n" + address->windowPath
      : std::string{};
  const bool appChanged = appId != m_appId || m_appName.empty();
  const bool menuChanged = menuKey != m_menuKey;
  // An open menu belongs to the app it was opened for: it never acts on the next one.
  if ((appChanged || menuChanged) && m_menu != nullptr && m_menu->isOpen()) {
    m_menu->close();
  }
  if (appChanged) {
    m_appId = std::move(appId);
    const auto entry = app_identity::findDesktopEntry(m_appId, desktopEntries());
    m_appName = entry.has_value() && !entry->name.empty() ? entry->name : m_appId;
    m_itemsDirty = true;
  }
  if (menuChanged) {
    m_menuKey = std::move(menuKey);
    // The menus load async: the name and Window show at once, the menus replace Window when they arrive.
    const auto onChanged = [this] {
      m_itemsDirty = true;
      requestUpdate();
    };
    m_appMenu.reset();
    m_gtkMenu.reset();
    if (gtk) {
      m_gtkMenu = std::make_shared<GtkMenuClient>(
          m_bus->connection(), address->service, address->path, address->applicationPath, address->windowPath,
          onChanged
      );
    } else if (hasMenu) {
      m_appMenu = std::make_shared<DbusMenuClient>(m_bus->connection(), address->service, address->path, onChanged);
    }
    m_itemsDirty = true;
  }
  // A reload while a menu is open (AboutToShow, org.gtk.Menus Changed) waits for the close: a rebuild closes it.
  if (!m_itemsDirty || (m_menu != nullptr && m_menu->isOpen())) {
    return;
  }
  rebuildItems();
  if (auto* node = root(); node != nullptr) {
    node->setVisible(!m_appId.empty());
  }
  // The name changes the width: measure now so the bar lays out with it.
  doLayout(renderer, 0.0F, 0.0F);
}

void WindowMenuWidget::doLayout(Renderer& renderer, float containerWidth, float /*containerHeight*/) {
  const float s = m_contentScale;
  if (containerWidth > 0.0F) {
    m_barWidth = containerWidth;
  }
  std::vector<float> widths(m_items.size(), 0.0F);
  float total = 0.0F;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    Item& item = m_items[i];
    if (item.area == nullptr || item.label == nullptr) {
      continue;
    }
    item.label->measure(renderer);
    widths[i] = std::round(item.label->width() + 2 * kItemPad * s);
    if (item.menuId != kOverflow) {
      total += widths[i];
    }
  }
  // The menus end before the bar's centre (the center island sits there); the rest go into ».
  const bool hasOverflow = !m_items.empty() && m_items.back().menuId == kOverflow;
  const float budget = m_barWidth * 0.5F - kCenterReserve * s;
  const bool overflow = hasOverflow && m_barWidth > 0.0F && total > budget;
  const float limit = overflow ? budget - widths.back() : std::numeric_limits<float>::max();
  float used = 0.0F;
  bool hiding = false;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    Item& item = m_items[i];
    if (item.menuId == kOverflow) {
      item.hidden = !overflow;
    } else {
      hiding = hiding || (i > 0 && used + widths[i] > limit);
      item.hidden = hiding;
      used += item.hidden ? 0.0F : widths[i];
    }
    if (item.area != nullptr) {
      item.area->setVisible(!item.hidden);
    }
  }
  float x = 0.0F;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    Item& item = m_items[i];
    if (item.area == nullptr || item.label == nullptr || item.hidden) {
      continue;
    }
    const float w = widths[i];
    const float h = kItemHeight * s;
    item.area->setPosition(x, 0.0F);
    item.area->setFrameSize(w, h);
    item.fill->setSize(w, h);
    item.label->setPosition(kItemPad * s, std::round((h - item.label->height()) * 0.5F));
    x += w;
  }
  if (auto* node = root(); node != nullptr) {
    node->setSize(x, kItemHeight * s);
  }
}

void WindowMenuWidget::syncFills() {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    Item& item = m_items[i];
    if (item.fill == nullptr) {
      continue;
    }
    const bool open = m_openItem == i && m_menu != nullptr && m_menu->isOpen();
    item.fill->setVisible(open || item.hovered);
    item.fill->setFill(colorSpecFromRole(open ? ColorRole::Fill2 : ColorRole::Fill1));
  }
  requestRedraw();
}

bool WindowMenuWidget::onPointerEvent(const PointerEvent& event) {
  if (m_menu == nullptr || !m_menu->isOpen()) {
    return false;
  }
  const bool consumed = m_menu->onPointerEvent(event);
  if (!consumed && event.type == PointerEvent::Type::Button && event.pressed) {
    m_menu->close();
    return true;
  }
  return consumed;
}

void WindowMenuWidget::openMenu(std::size_t item) {
  if (item >= m_items.size()) {
    return;
  }
  // Reopening fires the dismissed callback: it must belong to the menu that closes, not to the next one. A menu that
  // replaces an open one swaps at once (macOS menu tracking); only the first one grows.
  m_switchingMenu = m_menu != nullptr && m_menu->isOpen();
  if (m_switchingMenu) {
    m_menu->close();
  }
  if (m_items[item].menuId >= 0 || m_items[item].menuId == kOverflow) {
    openAppMenu(item);
    return;
  }
  const std::size_t index = item;
  const Item& menuItem = m_items[index];
  wl_surface* pointerSurface = m_platform.lastPointerSurface();
  auto* layerSurface = m_platform.layerSurfaceFor(pointerSurface);
  const auto windowId = m_platform.focusedCompositorWindowId();
  if (layerSurface == nullptr || menuItem.area == nullptr || !windowId.has_value()) {
    return;
  }
  m_windowId = *windowId;

  // Shortcuts are the default compositor bindings (examples/config.toml); Quit is the app's own Ctrl+Q.
  ContextMenuControlEntry moveTo{.label = i18n::tr("bar.window-menu.move-to"), .hasSubmenu = true};
  const auto workspaces = m_platform.workspaces(m_output);
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const auto& ws = workspaces[i];
    moveTo.children.push_back({
        .id = kWorkspaceBase + static_cast<std::int32_t>(i),
        .label = ws.name.empty() || ws.name == std::to_string(i + 1)
                     ? i18n::tr("bar.window-menu.workspace", "index", std::to_string(i + 1))
                     : ws.name,
        .checkmark = true,
        .toggleState = ws.active ? 1 : 0,
    });
  }
  std::vector<ContextMenuControlEntry> entries{
      {.id = Minimize, .label = i18n::tr("bar.window-menu.minimize")},
      {.id = Zoom, .label = i18n::tr("bar.window-menu.zoom"), .shortcut = "Super+Ctrl+F"},
      {.id = Fullscreen, .label = i18n::tr("bar.window-menu.fullscreen"), .shortcut = "Super+F"},
      {.separator = true},
      std::move(moveTo),
      {.separator = true},
      {.id = Close, .label = i18n::tr("bar.window-menu.close"), .shortcut = "Super+Q"},
      {.id = Quit, .label = i18n::tr("bar.window-menu.quit", "app", m_appName), .shortcut = "Ctrl+Q"},
  };

  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(m_platform.wayland(), m_renderContext);
  }
  m_menu->setShadowConfig(m_config.config().shell.shadow);
  m_menu->setOnDismissed([this] {
    m_openItem = static_cast<std::size_t>(-1);
    syncFills();
    requestUpdate();
  });
  m_menu->setOnActivate([this, workspaces, windowId = m_windowId, appId = m_appId](const ContextMenuControlEntry& entry) {
    auto& runtime = m_platform.andromedaRuntime();
    // The bar takes keyboard focus while the menu is open, so each action names its window.
    const auto onWindow = [&](std::string_view action) {
      (void)runtime.requestAction("window-focus:" + windowId);
      (void)runtime.requestAction(action);
    };
    switch (entry.id) {
    case Minimize:
      (void)runtime.requestAction("window-minimize:" + windowId);
      return;
    case Zoom:
      onWindow("window-toggle-maximize");
      return;
    case Fullscreen:
      onWindow("window-toggle-fullscreen");
      return;
    case Close:
      (void)runtime.requestAction("window-close:" + windowId);
      return;
    case Quit:
      for (const auto& window : m_platform.enrichedWindowsForApp(StringUtils::toLower(appId), {}, nullptr)) {
        m_platform.closeToplevelInfo(window);
      }
      return;
    default:
      break;
    }
    const auto ws = static_cast<std::size_t>(entry.id - kWorkspaceBase);
    if (entry.id >= kWorkspaceBase && ws < workspaces.size()) {
      // The window moves; the user stays on this workspace (macOS Move to).
      onWindow("window-move-to-workspace-silent:" + std::to_string(ws + 1));
    }
  });

  openPopup(index, std::move(entries));
}

// One top-level app menu: its rows, with a submenu shown as a caption over its indented rows (one level). The »
// item lists each menu that did not fit as a caption over its rows.
void WindowMenuWidget::openAppMenu(std::size_t item) {
  const Item& menuItem = m_items[item];
  const DbusMenuNode* menuRoot = appMenuRoot();
  wl_surface* pointerSurface = m_platform.lastPointerSurface();
  if (menuRoot == nullptr || m_platform.layerSurfaceFor(pointerSurface) == nullptr || menuItem.area == nullptr) {
    return;
  }
  std::vector<const DbusMenuNode*> tops;
  const auto topAt = [&](std::int32_t index) -> const DbusMenuNode* {
    return index >= 0 && static_cast<std::size_t>(index) < menuRoot->children.size()
        ? &menuRoot->children[static_cast<std::size_t>(index)]
        : nullptr;
  };
  std::vector<ContextMenuControlEntry> entries;
  if (menuItem.menuId == kOverflow) {
    for (const Item& hidden : m_items) {
      if (const DbusMenuNode* top = hidden.hidden ? topAt(hidden.menuId) : nullptr; top != nullptr) {
        tops.push_back(top);
        if (!entries.empty()) {
          entries.push_back({.separator = true});
        }
        entries.push_back({.label = top->label, .header = true});
        for (const DbusMenuNode& node : top->children) {
          addMenuRows(entries, node, 1);
        }
      }
    }
  } else if (const DbusMenuNode* top = topAt(menuItem.menuId); top != nullptr) {
    tops.push_back(top);
    for (const DbusMenuNode& node : top->children) {
      addMenuRows(entries, node, 0);
    }
  }
  // Before the empty check: a lazy dbusmenu app fills an empty menu on AboutToShow, and the reload rebuilds the bar.
  std::vector<std::int32_t> topIds;
  for (const DbusMenuNode* top : tops) {
    topIds.push_back(top->id);
    if (m_appMenu != nullptr) {
      m_appMenu->opened(top->id);
    }
  }
  if (entries.empty()) {
    return;
  }
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(m_platform.wayland(), m_renderContext);
  }
  m_menu->setShadowConfig(m_config.config().shell.shadow);
  const std::weak_ptr<DbusMenuClient> dbusClient = m_appMenu;
  const std::weak_ptr<GtkMenuClient> gtkClient = m_gtkMenu;
  m_menu->setOnDismissed([this, topIds, dbusClient] {
    if (const auto client = dbusClient.lock(); client != nullptr) {
      for (const std::int32_t topId : topIds) {
        client->closed(topId);
      }
    }
    m_openItem = static_cast<std::size_t>(-1);
    syncFills();
    requestUpdate();
  });
  m_menu->setOnActivate([dbusClient, gtkClient](const ContextMenuControlEntry& entry) {
    if (const auto client = dbusClient.lock(); client != nullptr) {
      client->activate(entry.id);
    } else if (const auto gtk = gtkClient.lock(); gtk != nullptr) {
      gtk->activate(entry.id);
    }
  });
  openPopup(item, std::move(entries));
}

void WindowMenuWidget::openPopup(std::size_t index, std::vector<ContextMenuControlEntry> entries) {
  const Item& menuItem = m_items[index];
  wl_surface* pointerSurface = m_platform.lastPointerSurface();
  auto* layerSurface = m_platform.layerSurfaceFor(pointerSurface);
  if (layerSurface == nullptr || menuItem.area == nullptr) {
    return;
  }
  float absX = 0.0F;
  float absY = 0.0F;
  Node::absolutePosition(menuItem.area, absX, absY);
  const bool bottom = m_barPosition == "bottom";
  const auto gap = static_cast<std::int32_t>(std::lround(Style::spaceXs * m_contentScale));
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .minMenuWidth = kMenuWidth * m_contentScale,
      .maxMenuWidth = Style::menuAutoMaxWidth * m_contentScale,
      .maxVisible = 32,
      .anchor =
          PopupAnchorRect{
              .x = static_cast<std::int32_t>(std::lround(absX)),
              .y = static_cast<std::int32_t>(std::lround(absY)),
              .width = static_cast<std::int32_t>(std::lround(menuItem.area->width())),
              .height = static_cast<std::int32_t>(std::lround(menuItem.area->height())),
          },
      .parent = PopupSurfaceParent{.layerSurface = layerSurface, .output = m_output, .wlSurface = pointerSurface},
      // macOS menus drop from the item's leading edge.
      .placement =
          ContextMenuPopupPlacement{
              .anchor = bottom ? XDG_POSITIONER_ANCHOR_TOP_LEFT : XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
              .gravity = bottom ? XDG_POSITIONER_GRAVITY_TOP_RIGHT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
              .offsetY = bottom ? -gap : gap,
              .chromeAttachment =
                  popup_chrome::Attachment{
                      .horizontal = popup_chrome::HorizontalAttachment::Left,
                      .vertical = bottom ? popup_chrome::VerticalAttachment::Bottom : popup_chrome::VerticalAttachment::Top,
                  },
          },
      .grow = !m_switchingMenu,
  });
  m_menu->setOnNavigate([this](int delta) {
    if (m_openItem >= m_items.size()) {
      return;
    }
    const std::size_t count = m_items.size();
    std::size_t next = m_openItem;
    for (std::size_t step = 0; step < count; ++step) {
      next = delta > 0 ? (next + 1) % count : (next + count - 1) % count;
      if (!m_items[next].hidden) {
        openMenu(next);
        return;
      }
    }
  });
  m_openItem = index;
  syncFills();
}
