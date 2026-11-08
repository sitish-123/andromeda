#include "app/app_window.h"

#include "app/foreign_parent.h"

#include "app/app_loop.h"
#include "app/command_table.h"
#include "app/state_store.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "core/input/key_chord.h"
#include "ui/controls/button.h"
#include "app/app_popover.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "cursor-shape-v1-client-protocol.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr float kContextMenuMinWidth = 240.0F; // logical px: labels and their shortcut column fit
  constexpr Logger kLog("app-window");

  // Rulebook §9.12 app header.
  constexpr float kHeaderHeight = 48.0F;
  constexpr float kNavButton = 32.0F;
  constexpr float kNavInset = 8.0F;
  constexpr float kHeaderGap = 8.0F;
  constexpr float kTitleExtra = 4.0F; // mockup .title margin-left
  constexpr float kWindowButton = 24.0F;
  constexpr float kWindowButtonInset = 12.0F;
  constexpr float kWindowButtonGap = 8.0F;
  constexpr float kWindowButtonSymbol = 12.0F;
  constexpr float kMaximizeSymbol = 10.0F; // the square glyph fills its box: 10 draws the mockup's 7 px square
  constexpr float kMoreDot = 2.0F;         // mockup #more: three small points 7 apart
  constexpr float kMoreDotGap = 5.0F;
  // Rulebook §9.12: an 8 px band outside the window, 16 x 16 corners. The compositor starts an edge resize only with
  // Mod held (cursor.cpp), so the band is a transparent surface margin outside the xdg window geometry.
  constexpr float kResizeBand = 8.0F;
  constexpr float kResizeCorner = 16.0F;

  std::uint32_t cursorForEdges(std::uint32_t edges) {
    switch (edges) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    default:
      return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    }
  }

  float envUiScale() {
    if (const char* value = std::getenv("ANDROMEDA_UI_SCALE"); value != nullptr) {
      const float scale = std::strtof(value, nullptr);
      if (scale >= 0.5F && scale <= 4.0F) {
        return scale;
      }
    }
    return 1.0F;
  }

  // The header row (rulebook 9.12): the title keeps its natural width while the accessory (search) shrinks to its
  // minWidth; only past that does the title ellipsize.
  class HeaderRow final : public Flex {
  public:
    Label* title = nullptr;
    Flex* titleRow = nullptr;
    Node* accessory = nullptr;

  protected:
    void doLayout(Renderer& renderer) override {
      fit(renderer, width());
      Flex::doLayout(renderer);
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      fit(renderer, rect.width);
      Flex::doArrange(renderer, rect);
    }

  private:
    void fit(Renderer& renderer, float w) {
      if (title == nullptr || w <= 0.0F) {
        return;
      }
      float used = paddingLeft() + paddingRight();
      int shown = 0;
      for (const auto& child : children()) {
        if (!child->visible() || !child->participatesInLayout()) {
          continue;
        }
        ++shown;
        if (child.get() == titleRow) {
          used += titleRow->paddingLeft();
        } else if (child.get() == accessory) {
          auto* flex = dynamic_cast<Flex*>(accessory);
          used += flex != nullptr ? flex->minWidth() : child->measure(renderer, {}).width;
        } else {
          used += child->measure(renderer, {}).width;
        }
      }
      used += gap() * static_cast<float>(std::max(0, shown - 1));
      title->setMaxWidth(std::max(1.0F, w - used));
    }
  };

  Button::ButtonPalette windowButtonPalette(bool close) {
    const ColorSpec clear = clearColorSpec();
    const ColorSpec label = colorSpecFromRole(ColorRole::Label2);
    Button::ButtonPalette p;
    p.normal = {.bg = clear, .border = clear, .label = label};
    p.hover = close ? Button::ButtonStateColors{.bg = colorSpecFromRole(ColorRole::Danger), .border = clear,
                                               .label = fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 1.0F))}
                    : Button::ButtonStateColors{.bg = colorSpecFromRole(ColorRole::Fill1), .border = clear, .label = label};
    p.pressed = close ? p.hover
                      : Button::ButtonStateColors{.bg = colorSpecFromRole(ColorRole::Fill2), .border = clear, .label = label};
    p.disabled = p.normal;
    return p;
  }
} // namespace

AppWindow::AppWindow(AppLoop& loop, CommandTable& commands, StateStore& state, Config config)
    : m_loop(loop), m_commands(commands), m_state(state), m_config(std::move(config)), m_uiScale(envUiScale()) {}

AppWindow::~AppWindow() { close(); }

std::string AppWindow::key(const char* field) const { return "/window/" + m_config.stateKey + "/" + field; }

wl_surface* AppWindow::wlSurface() const noexcept { return m_surface != nullptr ? m_surface->wlSurface() : nullptr; }

bool AppWindow::ownsSurface(wl_surface* surface) const noexcept {
  return surface != nullptr &&
         (surface == wlSurface() || (m_menu != nullptr && m_menu->isOpen() && m_menu->wlSurface() == surface) ||
          (m_popover != nullptr && m_popover->isOpen() && m_popover->wlSurface() == surface));
}

void AppWindow::openMoreMenu() {
  if (m_surface == nullptr || m_moreButton == nullptr) {
    return;
  }
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(m_loop.wayland(), m_loop.renderContext());
  }
  // One header per group in menu-bar order, then each group's commands in registration order.
  std::vector<const AppCommand*> commands;
  for (const AppCommand& c : m_commands.commands()) {
    if (c.run && !c.label.empty() && c.group != "Window") {
      commands.push_back(&c);
    }
  }
  std::vector<std::string> groups{"File", "Edit", "View", "Go"};
  for (const AppCommand* c : commands) {
    if (std::ranges::find(groups, c->group) == groups.end()) {
      groups.push_back(c->group);
    }
  }
  std::vector<ContextMenuControlEntry> entries;
  for (const std::string& group : groups) {
    bool first = true;
    for (std::size_t i = 0; i < commands.size(); ++i) {
      const AppCommand& c = *commands[i];
      if (c.group != group) {
        continue;
      }
      if (first) {
        if (!entries.empty()) {
          entries.push_back({.separator = true});
        }
        entries.push_back({.label = group, .header = true});
        first = false;
      }
      entries.push_back({.id = static_cast<std::int32_t>(i + 1),
                         .label = c.label,
                         .shortcut = c.shortcut ? keyChordDisplayLabel(*c.shortcut) : std::string(),
                         .enabled = !c.enabled || c.enabled()});
    }
  }
  if (entries.empty()) {
    return;
  }
  std::vector<std::string> ids;
  ids.reserve(commands.size());
  for (const AppCommand* c : commands) {
    ids.push_back(c->id);
  }
  m_menu->setOnActivate([this, ids](const ContextMenuControlEntry& entry) {
    if (entry.id > 0 && static_cast<std::size_t>(entry.id) <= ids.size()) {
      DeferredCall::callLater([this, id = ids[static_cast<std::size_t>(entry.id) - 1], alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired()) {
          m_commands.run(id);
        }
      });
    }
  });
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(m_moreButton, x, y);
  const auto margin = static_cast<float>(m_surface->margin());
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .contentScale = m_uiScale,
      .maxVisible = 24,
      .anchor = PopupAnchorRect{.x = static_cast<std::int32_t>(x - margin), .y = static_cast<std::int32_t>(y - margin),
                                .width = static_cast<std::int32_t>(m_moreButton->width()),
                                .height = static_cast<std::int32_t>(m_moreButton->height())},
      .parent = PopupSurfaceParent{.xdgSurface = m_surface->xdgSurface(), .wlSurface = m_surface->wlSurface()},
  });
}

void AppWindow::openContextMenu(std::vector<ContextMenuControlEntry> entries, float sceneX, float sceneY,
                                std::function<void(std::int32_t id)> activated) {
  if (m_surface == nullptr || entries.empty()) {
    return;
  }
  if (m_menu == nullptr) {
    m_menu = std::make_unique<ContextMenuPopup>(m_loop.wayland(), m_loop.renderContext());
  }
  m_menu->setOnActivate([this, activated = std::move(activated)](const ContextMenuControlEntry& entry) {
    // The popup is still in its own callback: the command runs after it.
    DeferredCall::callLater([activated, id = entry.id, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        activated(id);
      }
    });
  });
  const auto margin = static_cast<float>(m_surface->margin());
  m_menu->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .minMenuWidth = kContextMenuMinWidth * m_uiScale,
      .contentScale = m_uiScale,
      .maxVisible = 24,
      .anchor = PopupAnchorRect{.x = static_cast<std::int32_t>(sceneX - margin),
                                .y = static_cast<std::int32_t>(sceneY - margin), .width = 1, .height = 1},
      .parent = PopupSurfaceParent{.xdgSurface = m_surface->xdgSurface(), .wlSurface = m_surface->wlSurface()},
  });
}

void AppWindow::openPopover(const Node* anchor, float width, float height,
                            std::function<std::unique_ptr<Node>(float scale)> content) {
  if (m_surface == nullptr || anchor == nullptr) {
    return;
  }
  if (m_popover == nullptr) {
    m_popover = std::make_unique<AppPopover>(m_loop.wayland(), m_loop.renderContext());
  }
  float x = 0.0F;
  float y = 0.0F;
  Node::absolutePosition(anchor, x, y);
  const auto margin = static_cast<float>(m_surface->margin());
  m_popover->open({.parentXdg = m_surface->xdgSurface(), .parentWl = m_surface->wlSurface(),
                   .serial = m_loop.wayland().lastInputSerial(),
                   .anchorX = static_cast<std::int32_t>(x - margin), .anchorY = static_cast<std::int32_t>(y - margin),
                   .anchorWidth = static_cast<std::int32_t>(anchor->width()),
                   .anchorHeight = static_cast<std::int32_t>(anchor->height()), .width = width * m_uiScale,
                   .height = height * m_uiScale, .scale = m_uiScale, .content = std::move(content)});
}

void AppWindow::refreshPopover() {
  if (m_popover != nullptr) {
    m_popover->refresh();
  }
}

void AppWindow::closePopover() {
  if (m_popover != nullptr) {
    m_popover->close();
  }
}

bool AppWindow::popoverOpen() const noexcept { return m_popover != nullptr && m_popover->isOpen(); }

bool AppWindow::open() {
  if (m_surface != nullptr) {
    return true;
  }
  WaylandConnection& wayland = m_loop.wayland();
  wl_output* output = wayland.lastPointerOutput();
  if (output == nullptr && !wayland.outputs().empty()) {
    output = wayland.outputs().front().output;
  }

  const auto scaled = [this](std::uint32_t v) { return static_cast<std::uint32_t>(std::lround(static_cast<float>(v) * m_uiScale)); };
  const std::uint32_t minW = scaled(m_config.minWidth);
  const std::uint32_t minH = scaled(m_config.minHeight);
  const auto width = std::max(minW, m_state.get<std::uint32_t>(key("width"), scaled(m_config.width)));
  const auto height = std::max(minH, m_state.get<std::uint32_t>(key("height"), scaled(m_config.height)));
  const bool maximized = m_state.get<bool>(key("maximized"), false);

  m_surface = std::make_unique<ToplevelSurface>(wayland);
  m_surface->setRenderContext(&m_loop.renderContext());
  m_surface->setAnimationManager(&m_animations);
  m_surface->setClosedCallback([this] { onClosedByCompositor(); });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    saveGeometry();
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });
  m_surface->setStateCallback([this, wasMaximized = maximized, wasFullscreen = false,
                               wasActive = false](const ToplevelState& st) mutable {
    saveGeometry();
    if (st.activated != wasActive) {
      wasActive = st.activated;
      if (m_onActive) {
        m_onActive(st.activated);
      }
    }
    if (st.maximized != wasMaximized || st.fullscreen != wasFullscreen) {
      wasMaximized = st.maximized;
      wasFullscreen = st.fullscreen;
      m_rebuild = true;
      m_surface->requestLayout();
    }
  });
  m_surface->setPrepareFrameCallback([this](bool /*needsUpdate*/, bool needsLayout) { prepareFrame(needsLayout); });
  m_surface->setUpdateCallback([] {});

  const ToplevelSurfaceConfig cfg{
      .width = width,
      .height = height,
      .minWidth = minW,
      .minHeight = minH,
      .title = m_config.title,
      .appId = m_config.appId.c_str(),
      .maximized = maximized,
      .resizeMargin = static_cast<std::uint32_t>(std::lround(kResizeBand * m_uiScale)),
  };
  if (!m_surface->initialize(output, cfg)) {
    kLog.warn("failed to create the {} toplevel", m_config.appId);
    m_surface.reset();
    return false;
  }
  if (!m_config.parentWindow.empty()) {
    m_foreignParent = std::make_unique<desk::ForeignParent>(wayland.display());
    m_foreignParent->setParent(m_surface->wlSurface(), m_config.parentWindow);
  }
  m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_loop.wayland().setCursorShape(serial, shape);
  });
  m_rebuild = true;
  return true;
}

void AppWindow::close() {
  if (m_surface == nullptr) {
    return;
  }
  m_state.flush();
  m_input.setSceneRoot(nullptr);
  m_surface->setSceneRoot(nullptr);
  m_foreignParent.reset();
  m_menu.reset();
  m_popover.reset();
  // Nodes unload their textures through the surface's renderer: they go first.
  m_root.reset();
  m_titleLabel = nullptr;
  m_surface.reset();
  m_maximizeButton = nullptr;
}

void AppWindow::onClosedByCompositor() {
  // Out of the surface's own callback: destroying it inside would free the listener that runs.
  // Two windows can close in one dispatch; the first reap may destroy this one before its lambda runs.
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    // The callback may destroy this window: keep a copy and touch no member after it.
    const auto onClosed = m_onClosed;
    close();
    if (onClosed) {
      onClosed();
    }
  });
}

void AppWindow::saveGeometry() {
  if (m_surface == nullptr) {
    return;
  }
  const ToplevelState& st = m_surface->state();
  m_state.set(key("maximized"), st.maximized);
  // A maximized or fullscreen size is the compositor's, not the user's: keep the last free size. Tiled edges do not
  // tell: Andromeda with prefer_no_csd marks floating windows tiled on all four edges (view_lifecycle.cpp).
  if (!st.maximized && !st.fullscreen && m_surface->windowWidth() > 0 && m_surface->windowHeight() > 0) {
    m_state.set(key("width"), m_surface->windowWidth());
    m_state.set(key("height"), m_surface->windowHeight());
  }
}

void AppWindow::setTitle(std::string title) {
  m_config.title = std::move(title);
  if (m_titleLabel != nullptr && m_titleLabel->setText(m_config.title)) {
    invalidate();
  }
}

void AppWindow::requestRebuild() {
  m_rebuild = true;
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void AppWindow::invalidate() {
  if (m_surface == nullptr || m_root == nullptr) {
    return;
  }
  if (m_root->layoutDirty()) {
    m_surface->requestLayout();
  } else if (m_root->paintDirty()) {
    m_surface->requestRedraw();
  }
}

void AppWindow::toggleFullscreen() {
  if (m_surface != nullptr) {
    m_surface->setFullscreen(!m_surface->state().fullscreen);
  }
}

bool AppWindow::fullscreen() const noexcept { return m_surface != nullptr && m_surface->state().fullscreen; }

void AppWindow::activate(const std::string& token) { m_loop.wayland().activateSurface(wlSurface(), token); }

void AppWindow::toggleMaximized() {
  if (m_surface != nullptr) {
    m_surface->setMaximized(!m_surface->state().maximized);
  }
}

void AppWindow::prepareFrame(bool needsLayout) {
  if (m_surface == nullptr) {
    return;
  }
  const auto width = static_cast<float>(m_surface->width());
  const auto height = static_cast<float>(m_surface->height());
  if (width <= 0.0F || height <= 0.0F) {
    return;
  }
  m_loop.renderContext().makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();
  // A rebuild drops the pressed area: hold it until the button is released, so no click or drag is lost.
  if (m_root == nullptr || (m_rebuild && !m_buttonDown)) {
    m_rebuild = false;
    buildScene(renderer, width, height);
    return;
  }
  if (needsLayout || m_root->width() != width || m_root->height() != height) {
    const auto margin = static_cast<float>(m_surface->margin());
    m_root->setSize(width, height);
    for (const auto& child : m_root->children()) {
      child->setPosition(margin, margin);
      child->setSize(width - 2.0F * margin, height - 2.0F * margin);
    }
    m_root->layout(renderer);
  }
}

void AppWindow::refreshNavigation() {
  const std::array<const char*, 2> commands{"go.back", "go.forward"};
  for (std::size_t i = 0; i < m_navButtons.size(); ++i) {
    if (m_navButtons[i] != nullptr) {
      m_navButtons[i]->setEnabled(m_commands.isEnabled(commands[i]));
    }
  }
  invalidate();
}

std::unique_ptr<Flex> AppWindow::buildHeader() {
  const float s = m_uiScale;
  auto header = std::make_unique<HeaderRow>();
  header->setDirection(FlexDirection::Horizontal);
  header->setAlign(FlexAlign::Center);
  header->setGap(kHeaderGap * s);
  header->setPadding(0.0F, kNavInset * s);
  header->setFillWidth(true);
  header->setMinHeight(kHeaderHeight * s);
  header->setMaxHeight(kHeaderHeight * s);
  if (m_config.navigation) {
    std::size_t slot = 0;
    // Mockup .ib: 32 circles, label_1, disabled label_3.
    const ColorSpec clear = clearColorSpec();
    Button::ButtonPalette nav;
    nav.normal = {.bg = clear, .border = clear, .label = colorSpecFromRole(ColorRole::Label1)};
    nav.hover = {.bg = colorSpecFromRole(ColorRole::Fill1), .border = clear, .label = nav.normal.label};
    nav.pressed = {.bg = colorSpecFromRole(ColorRole::Fill2), .border = clear, .label = nav.normal.label};
    nav.disabled = {.bg = clear, .border = clear, .label = colorSpecFromRole(ColorRole::Label3)};
    for (const auto& [glyph, command] : {std::pair{"chevron-left", "go.back"}, std::pair{"chevron-right", "go.forward"}}) {
      header->addChild(ui::button({
          .out = &m_navButtons[slot++],
          .glyph = std::string(glyph),
          .glyphSize = Tokens::sizeSymbol * s,
          .enabled = m_commands.isEnabled(command),
          .variant = ButtonVariant::Ghost,
          .customPalette = nav,
          .radius = kNavButton * s / 2.0F,
          .width = kNavButton * s,
          .height = kNavButton * s,
          .onClick = [this, id = std::string(command)] { m_commands.run(id); },
      }));
    }
  }
  // Mockup .title: 600 17/24, 4 after the arrows; it ellipsizes in the middle only when the header is full.
  auto title = ui::row({.align = FlexAlign::Center, .clipChildren = true});
  header->titleRow = title.get();
  title->setPadding(0.0F, 0.0F, 0.0F, m_config.navigation ? kTitleExtra * s : 0.0F);
  title->addChild(ui::label({
      .out = &m_titleLabel,
      .text = m_config.title,
      .fontSize = Tokens::typeTitle2.size * s,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::Label1),
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
  }));
  header->title = m_titleLabel;
  header->addChild(std::move(title));
  if (m_headerAccessory) {
    // The accessory takes the free room (its search grows up to its maximum, then it right-aligns).
    auto accessory = m_headerAccessory();
    accessory->setFlexGrow(1.0F);
    header->accessory = accessory.get();
    header->addChild(std::move(accessory));
  } else {
    header->addChild(ui::row({.flexGrow = 1.0F}));
  }
  m_moreButton = nullptr;
  if (m_config.moreMenu) {
    const ColorSpec clear = clearColorSpec();
    Button::ButtonPalette ib;
    ib.normal = {.bg = clear, .border = clear, .label = colorSpecFromRole(ColorRole::Label1)};
    ib.hover = {.bg = colorSpecFromRole(ColorRole::Fill1), .border = clear, .label = ib.normal.label};
    ib.pressed = {.bg = colorSpecFromRole(ColorRole::Fill2), .border = clear, .label = ib.normal.label};
    ib.disabled = ib.normal;
    header->addChild(ui::button({
        .out = &m_moreButton,
        .variant = ButtonVariant::Ghost,
        .customPalette = ib,
        .tooltip = std::string("More"),
        .radius = kNavButton * s / 2.0F,
        .width = kNavButton * s,
        .height = kNavButton * s,
        .onClick = [this] { openMoreMenu(); },
    }));
    // The tabler "dots" glyph draws heavy points; the mockup's are small label_2 points.
    auto dots = ui::row({.align = FlexAlign::Center, .gap = kMoreDotGap * s});
    for (int i = 0; i < 3; ++i) {
      dots->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Label2), .radius = kMoreDot * s / 2.0F,
                              .width = kMoreDot * s, .height = kMoreDot * s}));
    }
    m_moreButton->addChild(std::move(dots));
  }

  auto buttons = ui::row({.align = FlexAlign::Center, .gap = kWindowButtonGap * s, .paddingH = (kWindowButtonInset - kNavInset) * s});
  const bool maximized = m_surface != nullptr && m_surface->state().maximized;
  const auto add = [&](const char* glyph, const char* command, bool close, Button** out) {
    buttons->addChild(ui::button({
        .out = out,
        .glyph = std::string(glyph),
        .glyphSize = (std::string_view(glyph) == "square" ? kMaximizeSymbol : kWindowButtonSymbol) * s,
        .customPalette = windowButtonPalette(close),
        .radius = kWindowButton * s / 2.0F,
        .width = kWindowButton * s,
        .height = kWindowButton * s,
        .onClick = [this, id = std::string(command)] { m_commands.run(id); },
    }));
  };
  add("minus", "window.minimize", false, nullptr);
  add(maximized ? "minimize" : "square", "window.maximize", false, &m_maximizeButton);
  add("x", "window.close", true, nullptr);
  header->addChild(std::move(buttons));
  return header;
}

void AppWindow::buildScene(Renderer& renderer, float width, float height) {
  m_input.setSceneRoot(nullptr);
  m_titleLabel = nullptr;
  m_maximizeButton = nullptr;
  m_navButtons = {};
  m_sidebarNode = nullptr;
  m_root = ui::node({});
  m_root->setSize(width, height);
  m_root->setAnimationManager(&m_animations);
  // The scene covers the surface; the window body sits inside the transparent resize margin.
  const auto margin = static_cast<float>(m_surface->margin());
  width -= 2.0F * margin;
  height -= 2.0F * margin;

  const float sidebarWidth = m_sidebarBuilder ? m_config.sidebarWidth * m_uiScale : 0.0F;
  const float sidebarShown = sidebarWidth * m_sidebarMix;
  const float mainWidth = std::max(0.0F, width - sidebarShown);
  auto main = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  const bool header = m_config.header && (m_config.headerWhenFullscreen || !fullscreen());
  if (header) {
    auto bar = buildHeader();
    if (m_config.transparentContent) {
      bar->setFill(colorSpecFromRole(ColorRole::SolidRegular));
    }
    main->addChild(std::move(bar));
  }
  if (!m_config.transparentContent) {
    main->setFill(colorSpecFromRole(ColorRole::SolidRegular));
  }
  const float headerHeight = header ? kHeaderHeight * m_uiScale : 0.0F;
  const float contentHeight = std::max(0.0F, height - headerHeight);
  m_contentRect = {.x = margin + sidebarShown, .y = margin + headerHeight,
                   .width = mainWidth, .height = contentHeight};
  std::unique_ptr<Node> content = m_builder ? m_builder(renderer, mainWidth, contentHeight) : ui::node({});
  auto holder = ui::column({.align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F});
  holder->addChild(std::move(content));
  main->addChild(std::move(holder));

  auto body = ui::row({.align = FlexAlign::Stretch, .width = width, .height = height});
  if (sidebarWidth > 0.0F) {
    // Mockup .side: a 0.5 px separator-colour hairline on the sidebar's own fill, so no gap shows the wallpaper.
    const float edgeW = 0.5F * m_uiScale;
    // Collapsing (setSidebarVisible) narrows this clip; the list inside keeps its width.
    auto sidebar = ui::row({.out = &m_sidebarNode, .align = FlexAlign::Stretch, .minWidth = sidebarShown,
                            .maxWidth = sidebarShown, .clipChildren = true, .width = sidebarShown});
    sidebar->setFill(colorSpecFromRole(ColorRole::SolidThin));
    auto list = ui::column({.align = FlexAlign::Stretch, .clipChildren = true, .width = sidebarWidth - edgeW});
    list->addChild(m_sidebarBuilder(renderer, sidebarWidth - edgeW, height));
    sidebar->addChild(std::move(list));
    sidebar->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator), .width = edgeW}));
    body->addChild(std::move(sidebar));
  }
  body->addChild(std::move(main));
  body->setSize(width, height);
  body->setPosition(margin, margin);
  body->layout(renderer);
  m_root->addChild(std::move(body));
  m_sheetCard = nullptr;
  if (m_sheetBuilder) {
    const float s = m_uiScale;
    auto scrim = ui::inputArea({});
    scrim->setPosition(margin, margin);
    scrim->setSize(width, height);
    auto shade = ui::box({.fill = colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? 0.20F : 0.40F),
                          .radius = Style::scaledRadius(Tokens::componentWindowRadius, s)});
    shade->setSize(width, height);
    scrim->addChild(std::move(shade));
    auto card = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Popover),
                            .radius = Style::scaledRadius(Tokens::componentDialogRadius, s), .clipChildren = true});
    card->addChild(m_sheetBuilder(s));
    card->layout(renderer);
    card->setPosition(std::round((width - card->width()) / 2.0F), std::round((height - card->height()) / 2.0F));
    card->setTransformOrigin(card->width() / 2.0F, card->height() / 2.0F);
    card->setOpacity(m_sheetMix);
    card->setScale(0.96F + 0.04F * m_sheetMix);
    m_sheetCard = card.get();
    scrim->addChild(std::move(card));
    m_root->addChild(std::move(scrim));
  }

  m_input.setSceneRoot(m_root.get());
  m_surface->setSceneRoot(m_root.get());
  if (m_onLayout) {
    m_onLayout();
  }
}

std::uint32_t AppWindow::resizeEdgesAt(float x, float y) const {
  // The margin is the band; maximized and fullscreen windows have none.
  const auto band = static_cast<float>(m_surface->margin());
  if (band <= 0.0F) {
    return 0;
  }
  const auto w = static_cast<float>(m_surface->width());
  const auto h = static_cast<float>(m_surface->height());
  const float corner = kResizeCorner * m_uiScale;
  const bool left = x < band;
  const bool right = x >= w - band;
  const bool top = y < band;
  const bool bottom = y >= h - band;
  // Corners are 16 x 16 even where the edge band is 8 deep.
  const bool nearLeft = x < corner;
  const bool nearRight = x >= w - corner;
  const bool nearTop = y < corner;
  const bool nearBottom = y >= h - corner;
  std::uint32_t edges = 0;
  if (top || ((left || right) && nearTop)) {
    edges |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
  } else if (bottom || ((left || right) && nearBottom)) {
    edges |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
  }
  if (left || ((top || bottom) && nearLeft)) {
    edges |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
  } else if (right || ((top || bottom) && nearRight)) {
    edges |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
  }
  return edges;
}

void AppWindow::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return;
  }
  if (m_popover != nullptr && m_popover->isOpen() && m_popover->onPointerEvent(event)) {
    return;
  }
  if (m_menu != nullptr && m_menu->isOpen()) {
    if (m_menu->onPointerEvent(event)) {
      return;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_menu->close();
    }
  }
  if (event.surface != nullptr && event.surface != wlSurface()) {
    return;
  }
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  const std::uint32_t edges = resizeEdgesAt(x, y);
  if (event.type == PointerEvent::Type::Button || event.type == PointerEvent::Type::Enter) {
    kLog.debug("pointer {} at {:.0f},{:.0f} of {}x{}, edges {}", event.type == PointerEvent::Type::Enter ? "enter" : "button",
               x, y, m_surface->windowWidth(), m_surface->windowHeight(), edges);
  }
  if (edges != 0 && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
    m_input.pointerLeave();
    m_loop.wayland().setCursorShape(event.serial, cursorForEdges(edges));
    m_inResizeBand = true;
    return;
  }
  if (edges != 0 && event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_LEFT) {
    m_surface->beginResize(event.serial, edges);
    return;
  }
  if (m_inResizeBand && event.type == PointerEvent::Type::Motion) {
    m_inResizeBand = false;
    m_loop.wayland().setCursorShape(event.serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    m_input.pointerEnter(x, y, event.serial);
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    m_input.pointerEnter(x, y, event.serial);
    // After a drag the pointer comes back with no release event: a rebuild held for the press runs now.
    m_buttonDown = false;
    if (m_rebuild) {
      m_surface->requestLayout();
    }
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    m_input.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button: {
    m_buttonDown = event.pressed;
    if (!event.pressed && m_rebuild) {
      m_surface->requestLayout();
    }
    m_input.pointerMotion(x, y, event.serial);
    const float windowY = y - static_cast<float>(m_surface->margin());
    const bool inHeader = windowY >= 0.0F && windowY < kHeaderHeight * m_uiScale && m_input.hoveredArea() == nullptr;
    if (event.pressed && inHeader && event.button == BTN_LEFT) {
      m_buttonDown = false;
      m_surface->beginMove(event.serial);
    } else if (event.pressed && inHeader && event.button == BTN_RIGHT) {
      m_buttonDown = false;
      m_surface->showWindowMenu(event.serial, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    } else {
      m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    }
    break;
  }
  case PointerEvent::Type::Axis:
    if (m_pointerInside) {
      m_input.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
      );
    }
    break;
  }
  invalidate();
}

bool AppWindow::requestSize(std::uint32_t width, std::uint32_t height) {
  if (m_surface == nullptr) {
    return false;
  }
  const auto scaled = [this](std::uint32_t v) { return static_cast<std::uint32_t>(std::lround(static_cast<float>(v) * m_uiScale)); };
  if (m_surface->requestWindowSize(scaled(width), scaled(height))) {
    return true;
  }
  // The compositor sizes this window: a larger minimum makes it grow (it stays the floor afterwards).
  m_surface->setMinSize(scaled(width), scaled(height));
  m_surface->requestRedraw();
  return false;
}

void AppWindow::setStateKey(std::string stateKey) {
  saveGeometry();
  m_config.stateKey = std::move(stateKey);
}

void AppWindow::setSidebarVisible(bool visible) {
  if (visible == m_sidebarVisible) {
    return;
  }
  m_sidebarVisible = visible;
  if (m_surface == nullptr) {
    m_sidebarMix = visible ? 1.0F : 0.0F; // before open: the first frame already has the final layout
    return;
  }
  MotionTraceLabel trace("window.sidebar");
  m_sidebarAnim = m_animations.drive(
      m_sidebarAnim, m_sidebarMix, visible ? 1.0F : 0.0F, Style::spring(Tokens::springPage),
      [this](float v) {
        m_sidebarMix = std::clamp(v, 0.0F, 1.0F);
        if (m_sidebarNode != nullptr && m_surface != nullptr) {
          const float w = m_config.sidebarWidth * m_uiScale * m_sidebarMix;
          m_sidebarNode->setMinWidth(w);
          m_sidebarNode->setMaxWidth(w);
          m_sidebarNode->markLayoutDirty();
          m_contentRect.x = static_cast<float>(m_surface->margin()) + w;
          m_contentRect.width = static_cast<float>(m_surface->windowWidth()) - w;
          m_surface->requestLayout();
        }
      },
      [this] { m_sidebarAnim = 0; }, nullptr, 0.0F, Unit::Px, true);
}

void AppWindow::presentSheet(std::function<std::unique_ptr<Node>(float)> builder, std::function<void()> onCancel) {
  m_sheetBuilder = std::move(builder);
  m_sheetCancel = std::move(onCancel);
  m_sheetMix = 0.0F;
  MotionTraceLabel trace("window.sheet");
  m_sheetAnim = m_animations.drive(
      m_sheetAnim, 0.0F, 1.0F, Style::spring(Tokens::springOpen),
      [this](float v) {
        m_sheetMix = v;
        if (m_sheetCard != nullptr) {
          m_sheetCard->setOpacity(v);
          m_sheetCard->setScale(0.96F + 0.04F * v);
        }
        invalidate();
      },
      [this] {
        m_sheetAnim = 0;
        m_sheetMix = 1.0F;
      },
      nullptr
  );
  requestRebuild();
}

void AppWindow::dismissSheet() {
  if (!m_sheetBuilder) {
    return;
  }
  if (m_sheetAnim != 0) {
    m_animations.cancel(m_sheetAnim);
    m_sheetAnim = 0;
  }
  m_sheetBuilder = {};
  m_sheetCancel = {};
  m_sheetMix = 1.0F;
  m_sheetCard = nullptr;
  requestRebuild();
}

void AppWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen()) {
    return;
  }
  if (m_menu != nullptr && m_menu->isOpen()) {
    m_menu->onKeyboardEvent(event);
    return;
  }
  if (m_popover != nullptr && m_popover->isOpen()) {
    m_popover->onKeyboardEvent(event);
    return;
  }
  if (m_sheetBuilder) {
    if (event.pressed && !event.preedit && event.sym == XKB_KEY_Escape) {
      if (auto cancel = m_sheetCancel) {
        cancel();
      } else {
        dismissSheet();
      }
      return;
    }
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    invalidate();
    return;
  }
  const InputArea* focused = m_input.focusedArea();
  const bool typing = focused != nullptr && focused->capturesAllKeys();
  // A focused text field owns plain keys; commands with Ctrl or Alt still run (Ctrl+W closes while typing).
  const bool commandKey = (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt)) != 0 || !typing;
  if (event.pressed && !event.preedit && commandKey && m_commands.handleKey(event.sym, event.modifiers)) {
    return;
  }
  if (!typing && m_keyHandler && m_keyHandler(event)) {
    invalidate();
    return;
  }
  m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  invalidate();
}
