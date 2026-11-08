#include "shell/launcher/launcher_panel.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"
#include "core/deferred_call.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/controls/virtual_list_view.h"
#include "wayland/drag_and_drop.h"
#include "wayland/drag_icon_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <cmath>

#include <glib.h>
#include <linux/input-event-codes.h>


bool LauncherPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr) {
    const bool onInput = (m_input != nullptr && focused == m_input->inputArea());
    const bool inResults = (m_grid != nullptr && isDescendantOf(focused, m_grid))
        || (m_launcherSections != nullptr && isDescendantOf(focused, m_launcherSections));
    if (!onInput && !inResults) {
      return false;
    }
  }

  if (m_categoryFilter != nullptr && m_categoryFilter->visible() && focused == m_categoryFilter->focusArea()) {
    return false;
  }

  return handleKeyEvent(sym, modifiers);
}

bool LauncherPanel::openAppActionsMenu(std::size_t index, float anchorX, float anchorY) {
  if (index >= m_results.size()) {
    return false;
  }
  const LauncherResult& base = m_results[index];

  const DesktopEntry* match = nullptr;
  for (const auto& e : desktopEntries()) {
    if (e.path == base.desktopEntryPath) {
      match = &e;
      break;
    }
  }
  if (match == nullptr) {
    return false;
  }

  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  if (wl == nullptr || rc == nullptr) {
    return false;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return false;
  }

  if (m_actionsMenu == nullptr) {
    m_actionsMenu = std::make_unique<ContextMenuPopup>(*wl, *rc);
  }

  std::vector<DesktopAction> actionsCopy = match->actions;
  const bool launcherPinned =
      m_config != nullptr && shell::dock::pinned_apps::containsEntry(m_config->config().shell.launcher.pinned, *match);
  const bool canPin = m_config != nullptr && !launcherPinned;
  const bool canUnpin = m_config != nullptr && launcherPinned;

  constexpr std::int32_t kActionOpen = -1;
  constexpr std::int32_t kActionPin = -2;
  constexpr std::int32_t kActionUnpin = -3;
  constexpr std::int32_t kActionAddToDesktop = -4;

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(actionsCopy.size() + 2);
  entries.push_back(
      ContextMenuControlEntry{
          .id = kActionOpen,
          .label = i18n::tr("launcher.context-menu.open"),
          .enabled = true,
          .separator = false,
          .hasSubmenu = false,
      }
  );
  // Desktop actions sit above pin/unpin so pin stays last in the menu.
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(actionsCopy.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = actionsCopy[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }
  if (canPin) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionPin,
            .label = i18n::tr("launcher.context-menu.pin"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  } else if (canUnpin) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionUnpin,
            .label = i18n::tr("launcher.context-menu.unpin"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }

  if (m_addToDesktop && m_config != nullptr && m_config->config().desktopIcons.enabled) {
    entries.push_back(ContextMenuControlEntry{.id = kActionAddToDesktop, .label = i18n::tr("launcher.context-menu.add-to-desktop")});
  }

  const float scale = contentScale();
  constexpr float kMenuWidth = 240.0F;
  const float minMenuWidth = kMenuWidth * scale;

  if (m_config != nullptr) {
    m_actionsMenu->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenu.get());

  m_actionsMenu->setOnDismissed([parentSurface = parentCtx->surface] {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_actionsMenu->setOnActivate([this, base, actionsCopy = std::move(actionsCopy),
                                entryForPin = *match](const ContextMenuControlEntry& entry) {
    LauncherResult result = base;
    if (entry.id == kActionAddToDesktop) {
      if (m_addToDesktop) {
        m_addToDesktop(entryForPin.path);
      }
      return;
    }
    if (entry.id == kActionPin) {
      if (m_config == nullptr
          || entryForPin.id.empty()
          || shell::dock::pinned_apps::containsEntry(m_config->config().shell.launcher.pinned, entryForPin)) {
        return;
      }
      std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
      pinned.push_back(entryForPin.id);
      if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (entry.id == kActionUnpin) {
      if (m_config == nullptr) {
        return;
      }
      std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
      shell::dock::pinned_apps::removeEntry(pinned, entryForPin);
      if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (entry.id >= 0 && entry.id < static_cast<std::int32_t>(actionsCopy.size())) {
      const DesktopAction& action = actionsCopy[static_cast<std::size_t>(entry.id)];
      result.id = AppProvider::actionResultId(result.desktopEntryPath, action.id);
      result.desktopActionId = action.id;
    } else if (entry.id != kActionOpen) {
      return;
    }

    for (auto& provider : m_providers) {
      if (provider->id() != std::string_view(result.providerId)) {
        continue;
      }
      if (!provider->activate(result)) {
        return;
      }
      finishActivation(*provider, result.id, provider->supportsAutoPaste(), !result.desktopEntryPath.empty());
      return;
    }
    return;
  });

  const float inset = std::round(std::max(4.0F, Style::spaceXs * scale));
  const auto ax = static_cast<std::int32_t>(std::round(anchorX - inset));
  const auto ay = static_cast<std::int32_t>(std::round(anchorY - inset));
  const auto aw = static_cast<std::int32_t>(std::round(inset * 2.0F));
  const auto ah = static_cast<std::int32_t>(std::round(inset * 2.0F));

  m_actionsMenu->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = minMenuWidth,
          .maxMenuWidth = Style::menuAutoMaxWidth * scale,
          .maxVisible = 12,
          .anchor =
              PopupAnchorRect{
                  .x = ax,
                  .y = ay,
                  .width = std::max(1, aw),
                  .height = std::max(1, ah),
              },
          .parent = PopupSurfaceParent{
              .layerSurface = parentCtx->layerSurface,
              .output = parentCtx->output,
          },
      }
  );
  return true;
}

void LauncherPanel::activateAt(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  selectResult(index, false);
  activateSelected();
}

void LauncherPanel::activateSelected() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }

  const auto& result = m_results[m_selectedIndex];
  if (result.providerId == "Files" && result.id == FilesProvider::kShowAllId) {
    setFilesScope(true);
    return;
  }
  if (result.providerId == kProviderOverviewProviderId && result.id.starts_with(kProviderOverviewResultPrefix)) {
    std::string prefix = result.id.substr(kProviderOverviewResultPrefix.size());
    if (!prefix.empty()) {
      prefix += ' ';
    }
    if (m_input != nullptr) {
      m_input->setValue(prefix);
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0F);
    }
    onInputChanged(prefix);
    return;
  }

  // Dispatch only to the provider that produced this result. Providers can use
  // overlapping id shapes, so probing every provider risks side effects.
  for (auto& provider : m_providers) {
    if (provider->id() != std::string_view(result.providerId)) {
      continue;
    }

    if (!result.desktopEntryPath.empty()) {
      const Node* cell = m_launcherSections != nullptr && m_launcherSections->visible()
          ? sectionCellForResult(*m_launcherSections, m_selectedIndex)
          : nullptr;
      if (cell == nullptr && m_grid != nullptr) {
        cell = m_grid->nodeForIndex(m_selectedIndex);
      }
      const Node* icon = cell != nullptr ? cellIcon(*cell) : nullptr;
      if (icon != nullptr) {
        PanelManager::instance().launchFrom(*icon);
      } else {
        kLog.debug("launch from: no icon cell for result {}", m_selectedIndex);
      }
    }
    if (!provider->activate(result)) {
      return;
    }

    finishActivation(*provider, result.id, provider->supportsAutoPaste(), !result.desktopEntryPath.empty());
    return;
  }
}

bool LauncherPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  const bool sectionNav =
      shouldUseLauncherSections() && m_launcherSectionsAdapter != nullptr && m_launcherSections != nullptr;
  const bool gridNav = !sectionNav && m_usingAppGrid && m_grid != nullptr;
  if (m_filesScope && sym == XKB_KEY_BackSpace && m_query.empty()) {
    setFilesScope(false);
    return true;
  }
  const int columns = gridNav ? static_cast<int>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;

  const auto moveSelection = [this](int delta) {
    if (m_results.empty()) {
      return;
    }
    const int last = static_cast<int>(m_results.size() - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    if (next == static_cast<int>(m_selectedIndex)) {
      return;
    }
    selectResult(static_cast<std::size_t>(next), true);
  };

  const auto moveSectionSelection = [this](bool vertical, int direction, std::size_t rows) {
    if (m_launcherSectionsAdapter == nullptr || m_results.empty()) {
      return;
    }
    const std::optional<std::size_t> next = vertical
        ? m_launcherSectionsAdapter->moveVertical(m_selectedIndex, direction, rows)
        : m_launcherSectionsAdapter->moveHorizontal(m_selectedIndex, direction);
    if (next.has_value()) {
      selectResult(*next, true);
    }
  };

  if (m_filesScope && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
    const bool back = sym == XKB_KEY_ISO_Left_Tab || (modifiers & KeyMod::Shift) != 0;
    setFilesKind(m_kindIndex + (back ? kFileKinds.size() - 1 : 1));
    return true;
  }
  if (!m_modeProviderId.empty() && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
    cycleMode((sym == XKB_KEY_ISO_Left_Tab || (modifiers & KeyMod::Shift) != 0) ? -1 : 1);
    return true;
  }

  if (KeySymbol::isPageUp(sym)) {
    if (sectionNav) {
      const std::size_t rows = m_launcherSectionsAdapter->estimatedVisibleApplicationRows(
          m_launcherSections->scrollView().contentViewportHeight()
      );
      moveSectionSelection(true, -1, rows);
      return true;
    }
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    if (sectionNav) {
      const std::size_t rows = m_launcherSectionsAdapter->estimatedVisibleApplicationRows(
          m_launcherSections->scrollView().contentViewportHeight()
      );
      moveSectionSelection(true, 1, rows);
      return true;
    }
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(true, -1, 1);
      return true;
    }
    moveSelection(gridNav ? -columns : -1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(true, 1, 1);
      return true;
    }
    moveSelection(gridNav ? columns : 1);
    return true;
  }

  if ((gridNav || sectionNav) && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(false, -1, 1);
    } else {
      moveSelection(-1);
    }
    return true;
  }

  if ((gridNav || sectionNav) && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (sectionNav) {
      moveSectionSelection(false, 1, 1);
    } else {
      moveSelection(1);
    }
    return true;
  }

  // Validate+Shift opens the app context menu (Shift layered on the configured
  // Validate chord). Menu navigation uses the Up/Down/Validate/Cancel keybinds.
  if ((modifiers & KeyMod::Shift) != 0
      && KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers & ~KeyMod::Shift)) {
    float anchorX = 0.0F;
    float anchorY = 0.0F;
    if (sectionNav && m_launcherSections != nullptr) {
      Node::absolutePosition(m_launcherSections, anchorX, anchorY);
      anchorX += m_launcherSections->width() * 0.5F;
      anchorY += m_launcherSections->height() * 0.5F;
    } else if (m_grid != nullptr && !m_grid->absoluteAnchorForIndex(m_selectedIndex, anchorX, anchorY)) {
      Node::absolutePosition(m_grid, anchorX, anchorY);
      anchorX += m_grid->width() * 0.5F;
      anchorY += m_grid->height() * 0.5F;
    }

    return openAppActionsMenu(m_selectedIndex, anchorX, anchorY);
  }

  // Ctrl+Y, not Space: Space types into the query.
  if (sym == XKB_KEY_y && (modifiers & ~KeyMod::Shift) == KeyMod::Ctrl && m_selectedIndex < m_results.size()
      && m_results[m_selectedIndex].providerId == "Files" && m_results[m_selectedIndex].id != FilesProvider::kShowAllId) {
    if (auto* files = filesProvider(); files != nullptr) {
      files->quickLook(m_results[m_selectedIndex]);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}

bool LauncherPanel::interceptPointerEvent(const PointerEvent& event, float width, float height) {
  if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT) {
    // Only a press on a result row arms it (not the search field); the scene selects that row after this, and the
    // first motion reads it.
    const auto onResults = [&](const Node* view) {
      if (view == nullptr || !view->visible() || view->width() <= 0.0F) {
        return false;
      }
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(view, x, y);
      return event.sx >= x && event.sx < x + view->width() && event.sy >= y && event.sy < y + view->height();
    };
    m_dragArmed = event.pressed && (onResults(m_launcherSections) || onResults(m_grid));
    if (event.pressed) {
      kLog.debug("launcher press {:.0f},{:.0f} on results {}", event.sx, event.sy, m_dragArmed);
    }
    m_dragIndex.reset();
    m_dragSerial = event.serial;
    return false;
  }
  if (event.type == PointerEvent::Type::Enter && m_selectedIndex < m_results.size()) {
    // Test hook (E2E desktop_shortcuts.sh): where the selected row is, in surface coordinates.
    const Node* cell = m_launcherSections != nullptr && m_launcherSections->visible()
        ? sectionCellForResult(*m_launcherSections, m_selectedIndex)
        : nullptr;
    if (cell == nullptr && m_grid != nullptr) {
      cell = m_grid->nodeForIndex(m_selectedIndex);
    }
    if (cell != nullptr) {
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(cell, x, y);
      kLog.debug("launcher selected row {} at {:.0f},{:.0f} {} {} {} {}", m_results[m_selectedIndex].title, event.sx,
                 event.sy, x, y, cell->width(), cell->height());
    }
  }
  if (event.type != PointerEvent::Type::Motion || !m_dragArmed) {
    return false;
  }
  if (!m_dragIndex.has_value()) {
    m_dragIndex = m_selectedIndex;
  }
  // Inside the launcher a drag reorders pinned apps or swipes grid pages; past its edge it carries the app out.
  const bool outside = event.sx < 0.0 || event.sy < 0.0 || event.sx >= width || event.sy >= height;
  if (outside) {
    kLog.debug("launcher drag left the panel with row {}", *m_dragIndex);
  }
  if (!outside || *m_dragIndex >= m_results.size() || m_results[*m_dragIndex].desktopEntryPath.empty()) {
    return false;
  }
  m_dragArmed = false;
  startAppDrag(m_results[*m_dragIndex]);
  return true;
}

void LauncherPanel::startAppDrag(const LauncherResult& result) {
  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  wl_surface* origin = PanelManager::instance().wlSurface();
  DragAndDrop* dnd = wl != nullptr ? wl->dragAndDrop() : nullptr;
  if (dnd == nullptr || rc == nullptr || origin == nullptr) {
    return;
  }
  gchar* uri = g_filename_to_uri(result.desktopEntryPath.c_str(), nullptr, nullptr);
  if (uri == nullptr) {
    return;
  }
  const std::string uris = std::string(uri) + "\r\n";
  g_free(uri);
  // Drag image: the app icon at the 64 desktop icon size.
  constexpr int kIcon = 64;
  m_dragIcon.reset();
  m_dragImage.reset();
  if (!result.iconPath.empty()) {
    auto icon = std::make_unique<DragIconSurface>(*wl, kIcon, kIcon, std::max(1, static_cast<int>(std::ceil(contentScale()))));
    icon->setRenderContext(rc);
    m_dragImage = ui::node({});
    m_dragImage->setSize(static_cast<float>(kIcon), static_cast<float>(kIcon));
    icon->setPrepareFrameCallback([this, rc, raw = icon.get(), file = result.iconPath, loaded = false](bool, bool) mutable {
      rc->makeCurrent(raw->renderTarget());
      if (!loaded && m_dragImage != nullptr) {
        loaded = true;
        auto image = ui::image({.fit = ImageFit::Contain, .width = static_cast<float>(kIcon),
                                .height = static_cast<float>(kIcon), .participatesInLayout = false});
        if (image->setSourceFile(raw->renderTarget().renderer(), file, kIcon, true)) {
          m_dragImage->addChild(std::move(image));
        }
      }
    });
    icon->setSceneRoot(m_dragImage.get());
    if (icon->initialize()) {
      m_dragIcon = std::move(icon);
    }
  }
  const std::string path = result.desktopEntryPath;
  // The shield under the launcher would take the drop: the desktop must see the drag.
  PanelManager::instance().dropClickShield();
  const bool started = dnd->startDrag(
      origin, m_dragSerial,
      {.mimeTypes = {"text/uri-list", "text/plain;charset=utf-8", "text/plain"},
       .data = [uris, path](const std::string& mime) { return mime == "text/uri-list" ? uris : path; },
       .actions = DragAndDrop::Copy,
       .icon = m_dragIcon != nullptr ? m_dragIcon->wlSurface() : nullptr,
       .finished =
           [this](std::uint32_t /*action*/, bool dropped) {
             kLog.debug("launcher app drag ended dropped {}", dropped);
             // The compositor may still draw the icon this frame: drop it after. The launcher closes either way
             // (Launchpad does): its outside-click shield is gone.
             DeferredCall::callLater([this] {
               m_dragIcon.reset();
               m_dragImage.reset();
               PanelManager::instance().closePanel();
             });
           }}
  );
  kLog.debug("launcher app drag {} {}", started ? "started" : "failed", path);
  if (!started) {
    m_dragIcon.reset();
    m_dragImage.reset();
    PanelManager::instance().closePanel();
  }
}
