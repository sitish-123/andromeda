#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/sheet/calendar_page.h"
#include "shell/control_center/sheet/power_menu_page.h"
#include "shell/control_center/sheet/quick_page.h"
#include "shell/control_center/sheet/weather_page.h"
#include "shell/panel/panel_button_style.h"
#include "render/animation/motion_service.h"
#include "shell/panel/panel_manager.h"
#include "system/dependency_service.h"
#include "system/easyeffects_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <wayland-client-protocol.h>

using namespace control_center;


namespace {

  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);
  // Full-height cards need room for rounded-rect AA before the tab viewport clips them.
  constexpr float kTabViewportClipInset = 1.0F;
  // The quick sheet: 360 px of cards plus the panel padding on both sides.
  TOKENS_LIVE(kQuickSheetWidth, Tokens::componentSheetWidth);

  float tabContentHeight(float viewportHeight) {
    return std::max(1.0F, std::floor(viewportHeight - kTabViewportClipInset));
  }

} // namespace

ControlCenterPanel::ControlCenterPanel(const ControlCenterServices& services) {
  m_config = services.config;
  m_mpris = services.mpris;
  m_notificationManager = services.notifications;
  m_dependencies = services.dependencies;
  m_tabs[tabIndex(TabId::Quick)] = std::make_unique<QuickPage>(services, [this](std::string_view tabKey) {
    selectTab(tabFromContext(tabKey), /*animated=*/true);
    PanelManager::instance().refresh();
  });
  m_tabs[tabIndex(TabId::Weather)] = std::make_unique<WeatherPage>(services.weather, services.config);
  m_tabs[tabIndex(TabId::Session)] = std::make_unique<PowerMenuPage>(services);
  m_tabs[tabIndex(TabId::Calendar)] = std::make_unique<CalendarPage>(services.config, services.calendar, services.weather);
  // A page opened straight from a bar icon closes on back (Esc), as a macOS / Windows flyout; one reached from the
  // quick sheet goes back to it.
  const auto back = [this] {
    if (m_openedOnDetail) {
      PanelManager::instance().close();
      return;
    }
    selectTab(TabId::Quick, /*animated=*/true);
    PanelManager::instance().refresh();
  };
  const auto addDetail = [this, &back](TabId id, auto page) {
    page->setOnBack(back);
    m_tabs[tabIndex(id)] = std::move(page);
  };
  addDetail(TabId::DetailWifi, std::make_unique<control_center::WifiDetailPage>(services.network, services.hotspot));
  addDetail(TabId::DetailBluetooth, std::make_unique<control_center::BluetoothDetailPage>(services.bluetooth, services.bluetoothAgent));
  addDetail(TabId::DetailSound, std::make_unique<control_center::SoundDetailPage>(services.audio, services.easyEffects));
  addDetail(TabId::DetailPower,
            std::make_unique<control_center::PowerDetailPage>(services.powerProfiles, services.upower));
  addDetail(TabId::DetailSearch, std::make_unique<control_center::SearchDetailPage>(services.network, [this](std::string_view key) {
    selectTab(tabFromContext(key), /*animated=*/true);
    PanelManager::instance().refresh();
  }));
  addDetail(TabId::DetailFocus, std::make_unique<control_center::FocusDetailPage>(services.notifications));
  addDetail(TabId::DetailPhone, std::make_unique<control_center::PhoneDetailPage>(services.link, services.notifications));
  addDetail(TabId::DetailDisplay,
            std::make_unique<control_center::DisplayDetailPage>(services.brightness, services.shortcutServices()));
  addDetail(TabId::DetailSystem, std::make_unique<control_center::SystemDetailPage>(services.sysmon, services.ipc));
  addDetail(TabId::DetailScreenTime, std::make_unique<control_center::ScreenTimeDetailPage>(services.screenTime));
  addDetail(TabId::DetailMedia, std::make_unique<control_center::MediaDetailPage>(services.mpris, services.httpClient));
  addDetail(TabId::DetailNotifications,
            std::make_unique<control_center::NotificationsDetailPage>(services.notifications, services.platform));
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
}

float ControlCenterPanel::preferredWidth() const { return kQuickSheetWidth * m_contentScale; }

bool ControlCenterPanel::fitsContentHeight() const noexcept { return true; }

float ControlCenterPanel::preferredHeight() const {
  // The sheet is the slot a detail page may grow into: the manager clamps it to the room it grants.
  return scaled(4096.0F);
}

float ControlCenterPanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (isDetailTab(m_activeTab) && m_contentHeader != nullptr && m_content != nullptr) {
    auto* page = dynamic_cast<control_center::DetailPage*>(m_tabs[tabIndex(m_activeTab)].get());
    if (page != nullptr) {
      return std::max(m_contentHeader->height(), Tokens::componentSheetDetailHeader * contentScale()) + m_content->gap() + m_content->paddingTop() + m_content->paddingBottom()
          + kTabViewportClipInset + page->measureHeight(renderer, contentWidth);
    }
  }
  if (m_activeTab == TabId::Calendar) {
    if (auto* calendar = dynamic_cast<CalendarPage*>(m_tabs[tabIndex(TabId::Calendar)].get()); calendar != nullptr) {
      return calendar->measureHeight(renderer, contentWidth);
    }
  }
  if (m_activeTab == TabId::Session) {
    if (auto* session = dynamic_cast<PowerMenuPage*>(m_tabs[tabIndex(TabId::Session)].get()); session != nullptr) {
      return session->measureHeight(renderer, contentWidth);
    }
  }
  if (m_activeTab == TabId::Weather) {
    if (auto* weather = dynamic_cast<WeatherPage*>(m_tabs[tabIndex(TabId::Weather)].get()); weather != nullptr) {
      return weather->measureHeight(renderer, contentWidth);
    }
  }
  auto* quick = dynamic_cast<QuickPage*>(m_tabs[tabIndex(TabId::Quick)].get());
  if (m_activeTab != TabId::Quick || quick == nullptr || m_contentHeader == nullptr || m_content == nullptr) {
    return preferredHeight();
  }
  return quick->measureHeight(renderer, contentWidth);
}

PanelPlacement ControlCenterPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.controlCenterPlacement;
}

bool ControlCenterPanel::dismissTransientUi() {
  const std::size_t activeIdx = tabIndex(m_activeTab);
  return m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi();
}

void ControlCenterPanel::create() {
  const float scale = contentScale();

  for (auto& tab : m_tabs) {
    tab->setContentScale(scale);
    tab->setPanelCardOpacity(panelCardOpacity());
  }

  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .padding = 0.0F,
  });

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .clipChildren = true,
      .flexGrow = 4.0F,
  });

  auto dismissArea = ui::inputArea({});
  dismissArea->setParticipatesInLayout(false);
  dismissArea->setZIndex(-1);
  dismissArea->setFocusable(false);
  dismissArea->setTabStop(false);
  dismissArea->setOnPress([this](const InputArea::PointerData&) {
    const std::size_t activeIdx = tabIndex(m_activeTab);
    if (m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi()) {
      PanelManager::instance().refresh();
    }
  });
  m_contentDismissArea = static_cast<InputArea*>(content->addChild(std::move(dismissArea)));

  auto header = ui::row({
      .out = &m_contentHeader,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  header->setMinHeight(Tokens::componentSheetDetailHeader * scale);
  header->addChild(ui::button({
      .out = &m_backButton,
      .glyph = "chevron-left",
      .onClick = [this] { selectTab(TabId::Quick, /*animated=*/true); },
      .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
  }));
  header->addChild(
      ui::label({
          .out = &m_contentTitle,
          .fontSize = Tokens::typeTitle3.size * scale,
          .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeTitle3.weight)),
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
          // The row gives the title the width the back button and actions leave (flexGrow), so it ellipsizes there.
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );

  auto headerActions = ui::row({
      .out = &m_contentHeaderActions,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto actions = m_tabs[i]->createHeaderActions();
    m_tabHeaderActions[i] = actions.get();
    if (actions != nullptr) {
      actions->setVisible(false);
      m_contentHeaderActions->addChild(std::move(actions));
    }
  }

  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .clipChildren = true,
      .flexGrow = 1.0F,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto container = m_tabs[i]->create();
    container->setFlexGrow(1.0F);
    container->setParticipatesInLayout(false);
    container->setVisible(false);
    m_tabContainers[i] = container.get();
    m_tabBodies->addChild(std::move(container));
  }

  content->addChild(std::move(bodies));
  rootLayout->addChild(std::move(content));
  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_firstOpenAfterCreate = true;
  selectTab(m_activeTab);
}

void ControlCenterPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& tab : m_tabs) {
    if (tab != nullptr) {
      tab->setPanelCardOpacity(opacity);
    }
  }
}

void ControlCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr || m_tabBodies == nullptr) {
    return;
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  const float contentInnerWidth =
      std::max(0.0F, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();
  const float bodyContentHeight = tabContentHeight(bodyHeight);

  if (m_contentDismissArea != nullptr) {
    m_contentDismissArea->setPosition(0.0F, 0.0F);
    m_contentDismissArea->setFrameSize(m_content->width(), m_content->height());
  }

  if (m_contentHeader != nullptr) {
    m_contentHeader->setSize(contentInnerWidth, 0.0F);
  }

  for (auto* container : m_tabContainers) {
    if (container != nullptr && container->visible()) {
      container->setSize(bodyWidth, bodyContentHeight);
    }
  }

  layoutTabContainers(bodyWidth, bodyHeight);

  const auto layoutTab = [this, bodyWidth, bodyContentHeight, &renderer](TabId tabId) {
    const std::size_t idx = tabIndex(tabId);
    if (m_tabs[idx] == nullptr || m_tabContainers[idx] == nullptr || !m_tabContainers[idx]->visible()) {
      return;
    }
    m_tabs[idx]->layout(renderer, bodyWidth, bodyContentHeight);
  };

  if (m_tabTransitionActive) {
    layoutTab(m_tabTransitionOutgoing);
  }
  layoutTab(m_activeTab);
}

void ControlCenterPanel::doUpdate(Renderer& renderer) {
  if (!isTabFeatureAvailable(m_activeTab)) {
    selectTab(TabId::Quick);
  }
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->update(renderer);
  }
  runTitleMove();
}

void ControlCenterPanel::onFrameTick(float deltaMs) {
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->onFrameTick(deltaMs);
  }
}

void ControlCenterPanel::onOpen(std::string_view context) {
  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }
  const bool animateTabSwitch = !m_firstOpenAfterCreate;
  m_firstOpenAfterCreate = false;
  const TabId tab = tabFromContext(context);
  m_openedOnDetail = isDetailTab(tab);
  selectTab(tab, animateTabSwitch);
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  return m_activeTab == tabFromContext(context);
}

bool ControlCenterPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (auto& tab = m_tabs[tabIndex(m_activeTab)]; tab != nullptr && tab->handleKey(sym, modifiers, pressed, preedit)) {
    return true;
  }
  return false;
}


void ControlCenterPanel::onClose() {
  if (m_titleMove.animation != 0 && m_animations != nullptr) {
    m_animations->cancel(m_titleMove.animation);
  }
  m_titleMove = {};
  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
  }
  m_tabTransitionActive = false;
  m_activeTab = TabId::Quick;
  for (auto& tab : m_tabs) {
    tab->setActive(false);
    tab->onClose();
  }
  m_rootLayout = nullptr;
  m_content = nullptr;
  m_contentDismissArea = nullptr;
  m_contentHeader = nullptr;
  m_contentHeaderActions = nullptr;
  m_contentTitle = nullptr;
  m_backButton = nullptr;
  m_tabBodies = nullptr;
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
  clearReleasedRoot();
}

bool ControlCenterPanel::isTabFeatureAvailable(TabId tab) const {
  return tab != TabId::Weather || m_config == nullptr || m_config->config().weather.enabled;
}

std::vector<ControlCenterPanel::LauncherTabEntry> ControlCenterPanel::visibleTabsForLauncher() const {
  std::vector<LauncherTabEntry> out;
  out.reserve(kTabCount - 1);
  for (const auto& meta : kTabs) {
    if (meta.id == TabId::Quick || meta.id == TabId::DetailSearch || !isTabFeatureAvailable(meta.id)) {
      continue;
    }
    out.push_back({.key = meta.key, .titleKey = meta.titleKey, .glyph = meta.glyph});
  }
  return out;
}

void ControlCenterPanel::updateTabChrome(TabId tab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabFeatureAvailable(meta.id);
    if (m_tabs[idx] != nullptr) {
      m_tabs[idx]->setActive(tabEnabled && meta.id == tab);
    }
    if (meta.id == tab && m_contentTitle != nullptr) {
      const std::string dynamicTitle = m_tabs[idx] != nullptr ? m_tabs[idx]->headerTitle() : std::string{};
      m_contentTitle->setText(!dynamicTitle.empty() ? dynamicTitle : i18n::tr(meta.titleKey));
    }
    if (m_tabHeaderActions[idx] != nullptr) {
      m_tabHeaderActions[idx]->setVisible(tabEnabled && meta.id == tab);
    }
  }

  // The quick, calendar, weather and power menu sheets draw their own headers.
  const bool ownHeader = tab == TabId::Quick || tab == TabId::Calendar || tab == TabId::Weather || tab == TabId::Session;
  if (m_backButton != nullptr) {
    m_backButton->setVisible(!ownHeader);
  }
  if (m_contentHeader != nullptr) {
    m_contentHeader->setVisible(!ownHeader);
  }
}

void ControlCenterPanel::applyTabContainerVisibility(TabId activeTab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabFeatureAvailable(meta.id);
    if (m_tabContainers[idx] != nullptr) {
      m_tabContainers[idx]->setVisible(tabEnabled && meta.id == activeTab);
    }
  }
}

void ControlCenterPanel::layoutTabContainers(float bodyWidth, float bodyHeight) {
  // P12: pages slide side by side with a 32 gap; a deeper level moves the lower page 30 % and dims it.
  constexpr const float& kPageGap = Tokens::spaceP32;
  constexpr float kParallax = 0.3F;
  const float travel = bodyWidth > 0.0F ? bodyWidth + kPageGap * m_contentScale : 0.0F;
  const float contentHeight = tabContentHeight(bodyHeight);
  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto* container = m_tabContainers[i];
    if (container == nullptr || !container->visible()) {
      continue;
    }

    container->setSize(bodyWidth, contentHeight);

    float offsetX = 0.0F;
    float opacity = 1.0F;
    int z = 0;
    const auto tabId = static_cast<TabId>(i);
    if (m_tabTransitionActive && travel > 0.0F) {
      const auto direction = static_cast<float>(m_tabTransitionDirection);
      const float p = m_tabTransitionProgress;
      const bool outgoing = tabId == m_tabTransitionOutgoing;
      const bool incoming = tabId == m_activeTab;
      if (m_tabTransitionDeeper) {
        // The detail page is the upper layer; the sheet is glass, so the lower page fades out under it.
        const bool forward = direction > 0.0F;
        const bool upper = forward ? incoming : outgoing;
        const float shown = forward ? p : 1.0F - p;
        if (upper && (incoming || outgoing)) {
          offsetX = bodyWidth * (1.0F - shown);
          z = 1;
        } else if (incoming || outgoing) {
          offsetX = -kParallax * bodyWidth * shown;
          opacity = 1.0F - shown;
        }
      } else if (outgoing) {
        offsetX = -direction * travel * p;
      } else if (incoming) {
        offsetX = direction * travel * (1.0F - p);
        z = 1;
      }
    }

    container->setPosition(offsetX, 0.0F);
    container->setOpacity(opacity);
    if (m_tabTransitionActive) {
      container->setZIndex(z);
    } else {
      container->setZIndex(0);
    }
  }
}

void ControlCenterPanel::resetTabContainerTransforms() {
  for (auto* container : m_tabContainers) {
    if (container == nullptr) {
      continue;
    }
    container->setPosition(0.0F, 0.0F);
    container->setOpacity(1.0F);
    container->setZIndex(0);
  }
}

int ControlCenterPanel::visibleTabOrdinal(TabId tab) const {
  int ordinal = 0;
  for (const auto& meta : kTabs) {
    if (!isTabFeatureAvailable(meta.id)) {
      continue;
    }
    if (meta.id == tab) {
      return ordinal;
    }
    ++ordinal;
  }
  return 0;
}

void ControlCenterPanel::applyTabTransitionLayout() {
  if (m_tabBodies == nullptr) {
    return;
  }
  layoutTabContainers(m_tabBodies->width(), m_tabBodies->height());
}

void ControlCenterPanel::startTabTransition(TabId from, TabId to) {
  if (m_animations == nullptr || m_tabBodies == nullptr) {
    applyTabContainerVisibility(to);
    resetTabContainerTransforms();
    return;
  }

  m_tabTransitionActive = true;
  m_tabTransitionOutgoing = from;
  m_tabTransitionProgress = 0.0F;

  const int fromOrdinal = visibleTabOrdinal(from);
  const int toOrdinal = visibleTabOrdinal(to);
  m_tabTransitionDeeper = (from == TabId::Quick && isDetailTab(to)) || (isDetailTab(from) && to == TabId::Quick);
  m_tabTransitionDirection = m_tabTransitionDeeper ? (isDetailTab(to) ? 1 : -1) : (toOrdinal >= fromOrdinal ? 1 : -1);

  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    if (m_tabContainers[idx] == nullptr) {
      continue;
    }
    const bool show = meta.id == from || meta.id == to;
    m_tabContainers[idx]->setVisible(show);
  }

  applyTabTransitionLayout();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
  PanelManager::instance().requestFrameTick();

  m_tabTransitionAnimId = m_animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springPage),
      [this](float progress) {
        m_tabTransitionProgress = progress;
        applyTabTransitionLayout();
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_tabTransitionAnimId = 0;
        finishTabTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_tabBodies
  , 0.0F, Unit::Progress);
}

void ControlCenterPanel::finishTabTransition() {
  m_tabTransitionActive = false;
  resetTabContainerTransforms();
  applyTabContainerVisibility(m_activeTab);
}

void ControlCenterPanel::selectTab(TabId tab, bool animated) {
  if (!isTabFeatureAvailable(tab)) {
    tab = TabId::Quick;
  }

  const TabId previousTab = m_activeTab;
  const bool tabChanged = tab != previousTab;

  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }

  endTitleMove();
  if (animated && previousTab == TabId::Quick && isDetailTab(tab) && m_tabs[tabIndex(previousTab)] != nullptr
      && MotionService::instance().enabled()) {
    if (Label* source = m_tabs[tabIndex(previousTab)]->sharedTitleFor(kTabs[tabIndex(tab)].key); source != nullptr) {
      beginTitleMove(*source);
    }
  }
  m_activeTab = tab;

  updateTabChrome(tab);

  if (tabChanged && animated && m_animations != nullptr && m_tabBodies != nullptr) {
    startTabTransition(previousTab, tab);
  } else {
    m_tabTransitionActive = false;
    applyTabContainerVisibility(tab);
    resetTabContainerTransforms();
  }

  scheduleMprisRefreshFor(tab);
}

void ControlCenterPanel::beginTitleMove(Label& source) {
  if (m_rootLayout == nullptr || m_contentTitle == nullptr || source.text().empty()) {
    return;
  }
  float sx = 0.0F;
  float sy = 0.0F;
  float rx = 0.0F;
  float ry = 0.0F;
  Node::absolutePosition(&source, sx, sy);
  Node::absolutePosition(m_rootLayout, rx, ry);
  auto overlay = std::make_unique<Label>();
  overlay->setParticipatesInLayout(false);
  overlay->setText(source.text());
  overlay->setMaxLines(1);
  overlay->setFontSize(Tokens::typeHeadline.size * m_contentScale);
  overlay->setLineHeight(Tokens::typeHeadline.line * m_contentScale);
  overlay->setFontWeight(static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)));
  overlay->setColor(colorSpecFromRole(ColorRole::Label1));
  overlay->setZIndex(100);
  overlay->setPosition(sx - rx, sy - ry);
  m_titleMove = TitleMove{
      .overlay = static_cast<Label*>(m_rootLayout->addChild(std::move(overlay))),
      .fromX = sx - rx,
      .fromY = sy - ry,
      .fromSize = Tokens::typeHeadline.size * m_contentScale,
      .fromLine = Tokens::typeHeadline.line * m_contentScale,
      .pending = true,
  };
  m_contentTitle->setOpacity(0.0F);
}

// The header has its place after the next layout: fly the overlay there, then hand over to the real title.
void ControlCenterPanel::runTitleMove() {
  if (!m_titleMove.pending || m_titleMove.overlay == nullptr || m_contentTitle == nullptr || m_rootLayout == nullptr
      || m_animations == nullptr) {
    return;
  }
  float tx = 0.0F;
  float ty = 0.0F;
  float rx = 0.0F;
  float ry = 0.0F;
  Node::absolutePosition(m_contentTitle, tx, ty);
  Node::absolutePosition(m_rootLayout, rx, ry);
  if (m_contentTitle->width() <= 0.0F) {
    constexpr int kMaxWaitFrames = 30;
    if (++m_titleMove.waitedFrames > kMaxWaitFrames) {
      endTitleMove();
    }
    return;
  }
  m_titleMove.pending = false;
  const float toX = tx - rx;
  const float toY = ty - ry;
  const float toSize = Tokens::typeTitle3.size * m_contentScale;
  const float toLine = Tokens::typeTitle3.line * m_contentScale;
  const TitleMove start = m_titleMove;
  m_titleMove.animation = m_animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springMove),
      [this, start, toX, toY, toSize, toLine](float p) {
        if (m_titleMove.overlay == nullptr) {
          return;
        }
        m_titleMove.overlay->setPosition(start.fromX + (toX - start.fromX) * p, start.fromY + (toY - start.fromY) * p);
        m_titleMove.overlay->setFontSize(start.fromSize + (toSize - start.fromSize) * p);
        m_titleMove.overlay->setLineHeight(start.fromLine + (toLine - start.fromLine) * p);
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_titleMove.animation = 0;
        endTitleMove();
      },
      m_rootLayout, 0.0F, Unit::Progress
  );
}

void ControlCenterPanel::endTitleMove() {
  if (m_titleMove.animation != 0 && m_animations != nullptr) {
    m_animations->cancel(m_titleMove.animation);
  }
  if (m_titleMove.overlay != nullptr && m_rootLayout != nullptr) {
    m_rootLayout->removeChild(m_titleMove.overlay);
  }
  m_titleMove = {};
  if (m_contentTitle != nullptr) {
    m_contentTitle->setOpacity(1.0F);
  }
}

void ControlCenterPanel::scheduleMprisRefreshFor(TabId tab) {
  if (m_mpris == nullptr || m_mprisRefreshScheduled || (tab != TabId::Quick && tab != TabId::DetailMedia)) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastMprisRefreshAt.time_since_epoch().count() != 0 && now - m_lastMprisRefreshAt < kMprisRefreshMinInterval) {
    return;
  }

  m_lastMprisRefreshAt = now;
  m_mprisRefreshScheduled = true;
  DeferredCall::callLater([this] {
    m_mprisRefreshScheduled = false;
    if (m_mpris == nullptr || !PanelManager::instance().isOpenPanel("control-center")) {
      return;
    }
    m_mpris->refreshPlayers();
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

ControlCenterPanel::TabId ControlCenterPanel::tabFromContext(std::string_view context) const {
  // Pre-v2 context names that user gesture config and scripts may still send; each opens its v2 page.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kOldContexts{{
      {"audio", "detail-sound"},
      {"bluetooth", "detail-bluetooth"},
      {"home", "quick"},
      {"notifications", "detail-notifications"},
      {"media", "detail-media"},
      {"screen-time", "detail-screen-time"},
      {"monitor", "detail-display"},
      {"network", "detail-wifi"},
      {"power", "detail-power"},
      {"system", "detail-system"},
  }};
  for (const auto& [old, current] : kOldContexts) {
    if (context == old) {
      context = current;
    }
  }
  for (const auto& tab : kTabs) {
    if (context == tab.key) {
      return tab.id;
    }
  }
  return TabId::Quick;
}

std::size_t ControlCenterPanel::tabIndex(TabId id) { return static_cast<std::size_t>(id); }

void ControlCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  // A horizontal view is the quick sheet's pager: it rests on whole pages, so a margin scroll would leave it 12 px off.
  if (ScrollView* scrollView = findEnclosingScrollView(area); scrollView != nullptr && scrollView->orientation() == ScrollOrientation::Vertical) {
    scrollNodeIntoScrollView(*scrollView, nullptr, *area, Style::spaceMd * contentScale());
    PanelManager::instance().requestLayout();
  }
}
