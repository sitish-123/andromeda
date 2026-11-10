#include "shell/control_center/sheet/calendar_page.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry_launch.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "calendar/calendar_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  // Month card (DESIGN.md "Clock calendar"): nav row 32, weekday row 24, day cells 36 with a 28 circle and a 4 dot.
  TOKENS_LIVE(kCalendarGridGap, Tokens::spaceP4);
  TOKENS_LIVE(kCalendarNavButtonSize, Tokens::sizeControlS);
  TOKENS_LIVE(kCalendarWeekdayRowHeight, Tokens::sizeControlS);
  TOKENS_LIVE(kCalendarHeaderHeight, Tokens::sizeControlM);
  TOKENS_LIVE(kCalendarDayCellHeight, Tokens::sizeControlM + Tokens::spaceP4);
  TOKENS_LIVE(kCalendarDayButtonSize, Tokens::sizeControlS + Tokens::spaceP4);
  // Three event rows show before the list scrolls.
  TOKENS_LIVE(kEventsHeight, Tokens::sizeRow * 3);
  constexpr float kCalendarLayoutEpsilon = 0.5F;
  // Week-number column: a fraction of a day column so it stays visually subordinate, floored at the
  // width of the two digits it holds (as a multiple of the caption font size) so it can never clip.
  constexpr float kCalendarWeekColumnRatio = 0.55F;
  constexpr float kCalendarWeekColumnMinFontScale = 1.45F;

} // namespace

CalendarPage::CalendarPage(ConfigService* config, CalendarService* calendar, WeatherService* weather)
    : m_config(config), m_calendar(calendar), m_weather(weather) {}

std::unique_ptr<Flex> CalendarPage::create() {
  const float scale = contentScale();

  if (m_calendar != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    (void)m_calendar->addChangeCallback([this] {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }

  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
    m_showWeekNumbers = m_config->config().controlCenter.calendarTab.showWeekNumbers;
  }

  // One-column clock sheet (.agent/DESIGN.md "Clock calendar + CC connectivity", direction A).
  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Tokens::componentSheetGap * scale,
  });

  tab->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP12 * scale, .paddingH = Tokens::spaceP4 * scale,
       .height = Tokens::componentSheetDetailHeader * scale},
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &m_weekdayLabel,
              .fontSize = Tokens::typeFootnote.size * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_todayLabel,
              .fontSize = Tokens::typeTitle2.size * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 1,
          })
      ),
      ui::button({
          .out = &m_todayButton,
          .text = i18n::tr("control-center.calendar.today"),
          .fontSize = Tokens::typeFootnote.size * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Tokens::sizeControlS * scale,
          .paddingH = Tokens::spaceP8 * scale,
          .radius = Tokens::radiusR8 * scale,
          .onClick =
              [this] {
                focusToday();
                PanelManager::instance().refresh();
              },
      })
  ));
  m_weekdayLabel->setLineHeight(Tokens::typeFootnote.line * scale);
  m_todayLabel->setLineHeight(Tokens::typeTitle2.line * scale);

  auto calendarArea = ui::inputArea({});
  calendarArea->setParticipatesInLayout(false);
  calendarArea->setPropagateEvents(true);
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return;
    }
    changeMonthBy(steps > 0.0F ? 1 : -1);
  });
  m_calendarArea = calendarArea.get();

  auto calendarCard = ui::column({
      .out = &m_card,
      .align = FlexAlign::Stretch,
      .gap = Tokens::spaceP4 * scale,
      .padding = Tokens::componentTilePadding * scale,
      .fill = colorSpecFromRole(ColorRole::GlassCard),
      .radius = Tokens::componentTileRadius * scale,
  });
  calendarCard->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  calendarCard->addChild(std::move(calendarArea));

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .gap = Tokens::spaceP4 * scale,
      .paddingH = Tokens::spaceP4 * scale,
      .height = kCalendarHeaderHeight * scale,
  });
  header->addChild(ui::column(
      {.out = &m_monthWrap, .align = FlexAlign::Start, .flexGrow = 1.0F},
      ui::label({
          .out = &m_monthLabel,
          .fontSize = Tokens::typeHeadline.size * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
      })
  ));
  const auto navButton = [scale](Button** out, const char* glyph, std::function<void()> onClick) {
    return ui::button({
        .out = out,
        .glyph = glyph,
        .glyphSize = Tokens::sizeSymbol * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = kCalendarNavButtonSize * scale,
        .minHeight = kCalendarNavButtonSize * scale,
        .padding = 0.0F,
        .radius = kCalendarNavButtonSize * scale * 0.5F,
        .onClick = std::move(onClick),
    });
  };
  header->addChild(navButton(&m_previousButton, Style::rtl() ? "chevron-right" : "chevron-left", [this] { changeMonthBy(-1); }));
  header->addChild(navButton(&m_nextButton, Style::rtl() ? "chevron-left" : "chevron-right", [this] { changeMonthBy(1); }));
  calendarCard->addChild(std::move(header));

  auto gridViewport = ui::column({
      .out = &m_gridViewport,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .height = gridHeight(scale),
      .configure = [](Flex& viewport) { viewport.setClipChildren(true); },
  });
  auto grid = ui::column({
      .out = &m_grid,
      .align = FlexAlign::Stretch,
      .gap = kCalendarGridGap * scale,
      .configure = [](Flex& layer) { layer.setParticipatesInLayout(false); },
  });
  gridViewport->addChild(std::move(grid));
  calendarCard->addChild(std::move(gridViewport));
  tab->addChild(std::move(calendarCard));

  // Events of the selected day: caption header (its date) over a grouped card.
  tab->addChild(ui::row(
      {.out = &m_eventsHeader, .paddingH = Tokens::spaceP16 * scale, .visible = m_showEventsCard},
      ui::label({
          .out = &m_eventsTitle,
          .fontSize = Tokens::typeCaption.size * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label3),
          .maxLines = 1,
      })
  ));
  auto eventsCard = ui::column(
      {.out = &m_eventsCard,
       .padding = Tokens::spaceP12 * scale,
       .fill = colorSpecFromRole(ColorRole::GlassCard),
       .radius = Tokens::componentTileRadius * scale,
       .visible = m_showEventsCard},
      ui::scrollView({
          .out = &m_eventsScroll,
          .contentScale = scale,
          .scrollbarAutoHide = true,
          .viewportPaddingH = Tokens::spaceP4 * scale,
          .fillWidth = true,
          .height = kEventsHeight * scale,
      })
  );
  eventsCard->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  tab->addChild(std::move(eventsCard));

  // Weather tile (span 2): opens the weather page; hidden while weather is off.
  auto weatherTile = ui::row(
      {.out = &m_weatherTile,
       .align = FlexAlign::Center,
       .gap = Tokens::componentTileGap * scale,
       .paddingH = Tokens::componentTilePadding * scale,
       .fill = colorSpecFromRole(ColorRole::GlassCard),
       .radius = Tokens::componentTileRadius * scale,
       .height = Tokens::componentTileHeight * scale,
       .visible = false},
      ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(ColorRole::Fill2),
           .radius = Tokens::componentRoundIconSize * scale * 0.5F,
           .width = Tokens::componentRoundIconSize * scale,
           .height = Tokens::componentRoundIconSize * scale},
          ui::glyph({
              .out = &m_weatherGlyph,
              .glyph = "weather-sun",
              .glyphSize = Tokens::componentRoundIconGlyph * scale,
              .color = colorSpecFromRole(ColorRole::Label1),
          })
      ),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({
              .out = &m_weatherTitle,
              .fontSize = Tokens::typeHeadline.size * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 1,
          }),
          ui::label({
              .out = &m_weatherDetail,
              .fontSize = Tokens::typeFootnote.size * scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      ),
      ui::glyph({
          .glyph = Style::rtl() ? "chevron-left" : "chevron-right",
          .glyphSize = Tokens::sizeSymbolInline * scale,
          .color = colorSpecFromRole(ColorRole::Label3),
      })
  );
  weatherTile->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  m_weatherTitle->setLineHeight(Tokens::typeHeadline.line * scale);
  m_weatherDetail->setLineHeight(Tokens::typeFootnote.line * scale);
  auto weatherArea = ui::inputArea({});
  weatherArea->setParticipatesInLayout(false);
  // Tile feedback (rulebook 9.3): hover fill_2, press fill_3.
  const auto tileFill = [this](ColorRole role) {
    if (m_weatherTile != nullptr) {
      m_weatherTile->setFill(colorSpecFromRole(role));
      PanelManager::instance().requestRedraw();
    }
  };
  weatherArea->setOnEnter([tileFill](const InputArea::PointerData&) { tileFill(ColorRole::Fill2); });
  weatherArea->setOnLeave([tileFill] { tileFill(ColorRole::GlassCard); });
  weatherArea->setOnPress([tileFill](const InputArea::PointerData& data) {
    tileFill(data.pressed ? ColorRole::Fill3 : ColorRole::Fill2);
  });
  weatherArea->setOnClick([](const InputArea::PointerData&) {
    PanelManager::instance().openPanel("control-center", PanelOpenRequest{.context = "weather"});
  });
  m_weatherArea = weatherArea.get();
  weatherTile->addChild(std::move(weatherArea));
  tab->addChild(std::move(weatherTile));
  updateWeather();

  return tab;
}

void CalendarPage::updateWeather() {
  if (m_weatherTile == nullptr) {
    return;
  }
  const bool show = m_weather != nullptr && m_weather->enabled() && m_weather->hasData();
  m_weatherTile->setVisible(show);
  if (!show) {
    return;
  }
  const WeatherSnapshot& snap = m_weather->snapshot();
  // Rebuild the text only when a new forecast arrived: doLayout calls this every frame the sheet lays out.
  if (snap.fetchedAt == m_weatherFetchedAt && !m_weatherTitle->text().empty()) {
    return;
  }
  m_weatherFetchedAt = snap.fetchedAt;
  const auto temp = [this](double c) { return static_cast<int>(std::lround(m_weather->displayTemperature(c))); };
  m_weatherGlyph->setGlyph(std::string(WeatherService::glyphForCode(snap.current.weatherCode, snap.current.isDay)));
  m_weatherTitle->setText(
      std::format("{}° {}", temp(snap.current.temperatureC), WeatherService::shortDescriptionForCode(snap.current.weatherCode))
  );
  std::string detail;
  if (!snap.forecastDays.empty()) {
    detail = i18n::tr(
        "control-center.calendar.weather-high-low", "high", std::to_string(temp(snap.forecastDays.front().temperatureMaxC)),
        "low", std::to_string(temp(snap.forecastDays.front().temperatureMinC))
    );
  }
  if (!snap.locationName.empty()) {
    detail += detail.empty() ? snap.locationName : " · " + snap.locationName;
  }
  m_weatherDetail->setText(detail);
}

std::unique_ptr<Flex> CalendarPage::createHeaderActions() {
  const float scale = contentScale();
  if (m_config != nullptr) {
    m_showEventsCard = m_config->config().controlCenter.calendarTab.showEventsCard;
  }
  return ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_toggleEventsCardButton,
          .glyph = m_showEventsCard ? "calendar-event" : "calendar-off",
          .selected = m_showEventsCard,
          .tooltip = i18n::tr("control-center.calendar.toggle-events-card"),
          .onClick = [this] { toggleEventsCard(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void CalendarPage::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr || m_card == nullptr || m_calendarArea == nullptr) {
    return;
  }

  updateWeather();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  m_calendarArea->setPosition(0.0F, 0.0F);
  m_calendarArea->setSize(m_card->width(), m_card->height());
  if (m_weatherArea != nullptr && m_weatherTile != nullptr) {
    m_weatherArea->setPosition(0.0F, 0.0F);
    m_weatherArea->setSize(m_weatherTile->width(), m_weatherTile->height());
  }
  if (m_todayButton != nullptr) {
    // No dead control: Today shows only when the view left today.
    const calendar_view::State now = calendar_view::stateForOffset(0);
    const bool onToday = m_monthOffset == 0 && m_selectedYear == now.current.year && m_selectedMonth == now.current.month
        && m_selectedDay == now.current.day;
    m_todayButton->setVisible(!onToday);
  }

  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float innerHeight = m_card->height();
  const calendar_view::State state = calendar_view::stateForOffset(m_monthOffset);

  // Default the selection to today until the user picks a day.
  if (m_selectedDay < 0) {
    m_selectedYear = state.current.year;
    m_selectedMonth = state.current.month;
    m_selectedDay = state.current.day;
  }

  const bool sizeChanged = std::abs(innerWidth - m_lastInnerWidth) >= kCalendarLayoutEpsilon
      || std::abs(innerHeight - m_lastInnerHeight) >= kCalendarLayoutEpsilon;
  const bool displayChanged = state.displayYear != m_lastDisplayYear || state.displayMonth != m_lastDisplayMonth;
  const bool todayChanged = state.current.year != m_lastCurrentYear
      || state.current.month != m_lastCurrentMonth
      || state.current.day != m_lastToday;

  if (m_monthSlideAnimId != 0 && !m_startMonthSlideIn) {
    m_rootLayout->layout(renderer);
    calendar_view::layoutEventLinkOverlays(m_eventListState);
    return;
  }

  if (!sizeChanged && !displayChanged && !todayChanged && !m_eventsDirty && !m_startMonthSlideIn) {
    return;
  }
  m_eventsDirty = false;

  m_lastInnerWidth = innerWidth;
  m_lastInnerHeight = innerHeight;
  m_lastDisplayYear = state.displayYear;
  m_lastDisplayMonth = state.displayMonth;
  m_lastCurrentYear = state.current.year;
  m_lastCurrentMonth = state.current.month;
  m_lastToday = state.current.day;

  rebuild();
  // The events card hugs its rows up to three, then scrolls.
  if (m_eventsScroll != nullptr && m_eventsScroll->content() != nullptr) {
    LayoutConstraints constraints;
    constraints.setExactWidth(std::max(0.0F, m_eventsScroll->width() - 2.0F * Tokens::spaceP4 * contentScale()));
    // Plus the inset under the last row, so a trailing button never touches the card edge.
    const float rows = m_eventsScroll->content()->measure(renderer, constraints).height + Tokens::spaceP4 * contentScale();
    const float height = std::clamp(rows, Tokens::sizeRowCompact * contentScale(), kEventsHeight * contentScale());
    if (std::abs(height - m_eventsScroll->height()) >= kCalendarLayoutEpsilon) {
      m_eventsScroll->setSize(m_eventsScroll->width(), height);
      PanelManager::instance().refresh();
    }
  }
  if (m_grid != nullptr) {
    if (m_monthSlideAnimId == 0 && !m_startMonthSlideIn) {
      m_grid->setPosition(0.0F, 0.0F);
      m_grid->setOpacity(1.0F);
    }
    m_grid->layout(renderer);
  }
  if (m_startMonthSlideIn && m_gridViewport != nullptr) {
    m_startMonthSlideIn = false;
    beginSlideIn();
  }
  m_rootLayout->layout(renderer);
  calendar_view::layoutEventLinkOverlays(m_eventListState);
}

void CalendarPage::doUpdate(Renderer& renderer) {
  (void)renderer;
  updateDateHeader();
}

void CalendarPage::updateDateHeader() {
  if (m_todayLabel == nullptr || m_weekdayLabel == nullptr) {
    return;
  }
  m_weekdayLabel->setText(formatLocalTime("%A"));
  m_todayLabel->setText(formatLocalTime("%-d %B"));
}

float CalendarPage::gridHeight(float scale) {
  return (kCalendarWeekdayRowHeight + kCalendarGridGap + 6.0F * kCalendarDayCellHeight + 5.0F * kCalendarGridGap) * scale;
}

float CalendarPage::measureHeight(Renderer& renderer, float width) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  updateWeather();
  m_rootLayout->setSize(width, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  return m_rootLayout->measure(renderer, constraints).height;
}

void CalendarPage::setActive(bool active) {
  if (!active) {
    m_eventFadeTimer.stop();
    return;
  }
  focusToday();
  if (!m_eventFadeTimer.active()) {
    m_eventFadeTimer.startRepeating(std::chrono::minutes{1}, [this] {
      m_eventsDirty = true;
      PanelManager::instance().refresh();
    });
  }
}

bool CalendarPage::handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  (void)modifiers;
  if (!pressed || preedit || m_rootLayout == nullptr) {
    return false;
  }
  // The full panel's sidebar navigates with arrows too: take keys only while focus is in this sheet (or nowhere).
  if (const InputArea* focused = PanelManager::instance().inputDispatcher().focusedArea(); focused != nullptr) {
    const Node* node = focused;
    while (node != nullptr && node != m_rootLayout) {
      node = node->parent();
    }
    if (node == nullptr) {
      return false;
    }
  }
  switch (sym) {
  case XKB_KEY_Left:
    moveSelection(Style::rtl() ? 1 : -1);
    return true;
  case XKB_KEY_Right:
    moveSelection(Style::rtl() ? -1 : 1);
    return true;
  case XKB_KEY_Up:
    moveSelection(-7);
    return true;
  case XKB_KEY_Down:
    moveSelection(7);
    return true;
  case XKB_KEY_Page_Up:
    changeMonthBy(-1);
    return true;
  case XKB_KEY_Page_Down:
    changeMonthBy(1);
    return true;
  case XKB_KEY_t:
  case XKB_KEY_T:
  case XKB_KEY_Home:
    focusToday();
    PanelManager::instance().refresh();
    return true;
  default:
    return false;
  }
}

void CalendarPage::moveSelection(int days) {
  using namespace std::chrono;
  if (m_selectedDay < 0) {
    return;
  }
  const year_month_day from{year{m_selectedYear} / month{static_cast<unsigned>(m_selectedMonth + 1)}
                            / day{static_cast<unsigned>(m_selectedDay)}};
  const year_month_day to{sys_days{from} + std::chrono::days{days}};
  m_selectedYear = static_cast<int>(to.year());
  m_selectedMonth = static_cast<int>(static_cast<unsigned>(to.month())) - 1;
  m_selectedDay = static_cast<int>(static_cast<unsigned>(to.day()));
  m_eventsDirty = true;
  // The shown month follows the selection: offset from today's month, not from the previous selection.
  cancelMonthSlide();
  const calendar_view::State now = calendar_view::stateForOffset(0);
  const int target = (m_selectedYear - now.current.year) * 12 + (m_selectedMonth - now.current.month);
  if (target != m_monthOffset) {
    changeMonthBy(target - m_monthOffset);
  } else {
    PanelManager::instance().refresh();
  }
}

void CalendarPage::focusToday() {
  cancelMonthSlide();
  const calendar_view::State state = calendar_view::stateForOffset(0);
  m_monthOffset = 0;
  m_selectedYear = state.current.year;
  m_selectedMonth = state.current.month;
  m_selectedDay = state.current.day;
  m_lastDisplayYear = std::numeric_limits<int>::min();
  m_lastDisplayMonth = -1;
  m_eventsDirty = true;
}

void CalendarPage::onClose() {
  m_eventFadeTimer.stop();
  cancelMonthSlide();
  m_rootLayout = nullptr;
  m_calendarArea = nullptr;
  m_card = nullptr;
  m_header = nullptr;
  m_previousSlot = nullptr;
  m_nextSlot = nullptr;
  m_monthWrap = nullptr;
  m_todayLabel = nullptr;
  m_monthLabel = nullptr;
  m_previousButton = nullptr;
  m_nextButton = nullptr;
  m_toggleEventsCardButton = nullptr;
  m_gridViewport = nullptr;
  m_grid = nullptr;
  m_eventsCard = nullptr;
  m_eventsTitle = nullptr;
  m_eventsScroll = nullptr;
  m_eventsHeader = nullptr;
  m_weekdayLabel = nullptr;
  m_weatherTile = nullptr;
  m_weatherGlyph = nullptr;
  m_weatherTitle = nullptr;
  m_weatherDetail = nullptr;
  m_weatherArea = nullptr;
  m_eventListState.linkOverlays.clear();
  m_selectedYear = std::numeric_limits<int>::min();
  m_selectedMonth = -1;
  m_selectedDay = -1;
  focusToday();
  m_eventsDirty = false;
  m_lastInnerWidth = -1.0F;
  m_lastInnerHeight = -1.0F;
  m_lastCurrentYear = std::numeric_limits<int>::min();
  m_lastCurrentMonth = -1;
  m_lastToday = -1;
}

void CalendarPage::changeMonthBy(int delta) {
  if (delta == 0) {
    return;
  }

  cancelMonthSlide();

  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  beginSlideOut(delta);
}

void CalendarPage::cancelMonthSlide() {
  if (m_monthSlideAnimId != 0 && m_gridViewport != nullptr) {
    if (AnimationManager* animations = m_gridViewport->animationManager(); animations != nullptr) {
      animations->cancel(m_monthSlideAnimId);
    }
    m_monthSlideAnimId = 0;
  }
  // A slide cut short still lands on its month: the next move counts from there.
  if (m_pendingMonthDelta != 0) {
    m_monthOffset += m_pendingMonthDelta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
  }
  m_pendingMonthDelta = 0;
  m_startMonthSlideIn = false;
  if (m_grid != nullptr) {
    m_grid->setPosition(0.0F, 0.0F);
    m_grid->setOpacity(1.0F);
  }
}

void CalendarPage::applyMonthSlide(float progress, bool slidingIn) {
  if (m_grid == nullptr || m_gridViewport == nullptr) {
    return;
  }

  const float travel = m_gridViewport->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_monthSlideDirection);
  if (slidingIn) {
    m_grid->setPosition(direction * travel * (1.0F - progress), 0.0F);
    m_grid->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_grid->setPosition(-direction * travel * progress, 0.0F);
    m_grid->setOpacity(1.0F - 0.3F * progress);
  }
}

void CalendarPage::beginSlideOut(int delta) {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    m_monthOffset += delta;
    m_lastDisplayYear = std::numeric_limits<int>::min();
    m_lastDisplayMonth = -1;
    PanelManager::instance().refresh();
    return;
  }

  m_monthSlideDirection = delta > 0 ? 1 : -1;
  m_pendingMonthDelta = delta;

  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springPage),
      [this](float progress) {
        applyMonthSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_monthSlideAnimId = 0;
        m_monthOffset += m_pendingMonthDelta;
        m_pendingMonthDelta = 0;
        m_lastDisplayYear = std::numeric_limits<int>::min();
        m_lastDisplayMonth = -1;
        m_startMonthSlideIn = true;
        PanelManager::instance().refresh();
      },
      m_gridViewport
  , 0.0F, Unit::Progress);
}

void CalendarPage::beginSlideIn() {
  AnimationManager* animations = m_gridViewport != nullptr ? m_gridViewport->animationManager() : nullptr;
  if (animations == nullptr || m_grid == nullptr) {
    return;
  }

  applyMonthSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_monthSlideAnimId = animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springPage),
      [this](float progress) {
        applyMonthSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_monthSlideAnimId = 0;
        if (m_grid != nullptr) {
          m_grid->setPosition(0.0F, 0.0F);
          m_grid->setOpacity(1.0F);
        }
      },
      m_gridViewport
  , 0.0F, Unit::Progress);
}

void CalendarPage::rebuild() {
  uiAssertNotRendering("CalendarPage::rebuild");
  if (m_grid == nullptr || m_monthLabel == nullptr || m_card == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float innerWidth = std::max(0.0F, m_card->width() - (m_card->paddingLeft() + m_card->paddingRight()));
  const float weekdayHeight = kCalendarWeekdayRowHeight * scale;
  const float dayCellHeight = kCalendarDayCellHeight * scale;
  // The week-number column takes its share of the row before the day columns split the rest. It is
  // inset from the divider by one grid gap, which reads as balanced against the card padding on its
  // other side without spending a full card padding's worth of width on a two-digit number.
  const float weekLaneInset = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F;
  const float weekDividerWidth = m_showWeekNumbers ? std::round(1.0F * scale) : 0.0F;
  const float weekLaneOverhead = m_showWeekNumbers ? weekLaneInset + weekDividerWidth + kCalendarGridGap * scale : 0.0F;
  // Solve for a day column that leaves the week column its fraction, then floor the week column at the
  // width of its text and give the day grid whatever is actually left.
  const float provisionalDayColumn = std::max(
      0.0F,
      (innerWidth - kCalendarGridGap * scale * 6.0F - weekLaneOverhead)
          / (m_showWeekNumbers ? 7.0F + kCalendarWeekColumnRatio : 7.0F)
  );
  const float weekColumnWidth = m_showWeekNumbers
      ? std::max(
            std::round(provisionalDayColumn * kCalendarWeekColumnRatio),
            std::round(Style::fontSizeCaption * scale * kCalendarWeekColumnMinFontScale)
        )
      : 0.0F;
  const float dayGridWidth = std::max(0.0F, innerWidth - weekColumnWidth - weekLaneOverhead);
  const float dayColumnWidth = std::max(0.0F, (dayGridWidth - kCalendarGridGap * scale * 6.0F) / 7.0F);
  const float dotDiameter = Tokens::spaceP4 * scale;
  const float dotGap = Tokens::spaceP2 * scale;
  const float dayButtonSize = std::min(kCalendarDayButtonSize * scale, dayColumnWidth);

  if (m_todayLabel != nullptr) {
    updateDateHeader();
  }

  calendar_view::rebuildMonth({
      .grid = *m_grid,
      .monthLabel = *m_monthLabel,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .monthOffset = m_monthOffset,
      .showWeekNumbers = m_showWeekNumbers,
      .scale = scale,
      .layout =
          {
              .width = innerWidth,
              .weekdayHeight = weekdayHeight,
              .dayCellHeight = dayCellHeight,
              .dayButtonSize = dayButtonSize,
              .gap = kCalendarGridGap * scale,
              .dotDiameter = dotDiameter,
              .dotGap = dotGap,
              .weekColumnWidth = weekColumnWidth,
              .weekLaneInset = weekLaneInset,
              .weekDividerWidth = weekDividerWidth,
              .weekDaysGap = m_showWeekNumbers ? kCalendarGridGap * scale : 0.0F,
          },
      .onDateSelected =
          [this](calendar_view::Date date, int monthShift) {
            m_selectedYear = date.year;
            m_selectedMonth = date.month;
            m_selectedDay = date.day;
            m_eventsDirty = true;
            if (monthShift != 0) {
              changeMonthBy(monthShift);
            } else {
              PanelManager::instance().refresh();
            }
          },
      .onDateRightClicked =
          [](calendar_view::Date) {
            if (desktop_entry_launch::launchDefaultForMimeType("text/calendar")) {
              PanelManager::instance().closePanel();
            }
          },
  });

  if (m_gridViewport != nullptr) {
    m_gridViewport->setSize(innerWidth, gridHeight(scale));
  }
  rebuildEventList(scale);
}

void CalendarPage::rebuildEventList(float scale) {
  if (m_eventsScroll == nullptr || m_config == nullptr) {
    return;
  }
  const auto& calendarConfig = m_config->config().calendar;
  const auto& tabConfig = m_config->config().controlCenter.calendarTab;
  calendar_view::rebuildEventList({
      .scroll = *m_eventsScroll,
      .title = m_eventsTitle,
      .snapshot = m_calendar != nullptr ? &m_calendar->snapshot() : nullptr,
      .selected = {.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay},
      .scale = scale,
      .dateFormat = calendarConfig.eventDateFormat,
      .timeFormat = calendarConfig.eventTimeFormat,
      .state = &m_eventListState,
      .hidePastEvents = tabConfig.hidePastEvents,
      .pastEventsThresholdMinutes = tabConfig.pastEventsThresholdMinutes,
      .onToggleHiddenPast =
          [this] {
            m_eventsDirty = true;
            PanelManager::instance().refresh();
          },
      .requestRedraw = [] { PanelManager::instance().requestRedraw(); },
  });
  // The sheet header already shows today's date: the caption says "Today" instead of repeating it.
  if (m_eventsTitle != nullptr) {
    const calendar_view::State now = calendar_view::stateForOffset(0);
    if (m_selectedYear == now.current.year && m_selectedMonth == now.current.month && m_selectedDay == now.current.day) {
      m_eventsTitle->setText(i18n::tr("control-center.calendar.today"));
    }
  }
  addEventsEmptyState(scale);
}

void CalendarPage::addEventsEmptyState(float scale) {
  Flex* content = m_eventsScroll != nullptr ? m_eventsScroll->content() : nullptr;
  if (content == nullptr || m_config == nullptr) {
    return;
  }
  const auto headline = [scale](std::string text, ColorRole role, const Tokens::Type& type, FontWeight weight,
                                int lines = 1) {
    auto label = ui::label({.text = std::move(text), .fontSize = type.size * scale, .fontWeight = weight,
                            .color = colorSpecFromRole(role), .maxLines = lines, .ellipsize = TextEllipsize::End});
    label->setLineHeight(type.line * scale);
    return label;
  };
  // No account: the card offers the set-up instead of an empty list.
  if (m_config->config().calendar.accounts.empty()) {
    while (!content->children().empty()) {
      content->removeChild(content->children().back().get());
    }
    content->setGap(Tokens::spaceP4 * scale);
    content->addChild(headline(i18n::tr("control-center.calendar.no-calendars"), ColorRole::Label1, Tokens::typeHeadline,
                               FontWeight::SemiBold));
    // Two lines, so the hint still reads at a large Text Size.
    content->addChild(headline(i18n::tr("control-center.calendar.no-calendars-body"), ColorRole::Label2,
                               Tokens::typeFootnote, FontWeight::Medium, 2));
    content->addChild(ui::row(
        {.paddingV = Tokens::spaceP4 * scale},
        ui::button({
            .text = i18n::tr("control-center.calendar.add-account"),
            .fontSize = Tokens::typeFootnote.size * scale,
            .variant = ButtonVariant::Default,
            .minHeight = Tokens::sizeControlS * scale,
            .paddingH = Tokens::spaceP8 * scale,
            .radius = Tokens::radiusR8 * scale,
            .onClick = [] { PanelManager::instance().openSettingsWindow("date-time"); },
        })
    ));
    return;
  }
  // A day without events also names the next upcoming one, so the card always answers "what is next".
  if (m_calendar == nullptr || !m_calendar->hasData()) {
    return;
  }
  using namespace std::chrono;
  // Same day keys as the list (calendar_view::eventDayRange): local days, DST-safe, all-day aware.
  const int selectedKey = calendar_view::dateKey({.year = m_selectedYear, .month = m_selectedMonth, .day = m_selectedDay});
  const CalendarEvent* next = nullptr;
  bool dayHasEvents = false;
  for (const CalendarEvent& event : m_calendar->snapshot().events) {
    const auto [first, last] = calendar_view::eventDayRange(event);
    if (first <= selectedKey && selectedKey <= last) {
      dayHasEvents = true;
      break;
    }
    if (first > selectedKey && (next == nullptr || event.start < next->start)) {
      next = &event;
    }
  }
  if (dayHasEvents || next == nullptr) {
    return;
  }
  const std::string when = formatLocalUnixTime(
      duration_cast<seconds>(next->start.time_since_epoch()).count(),
      next->allDay ? m_config->config().calendar.eventDateFormat
                   : m_config->config().calendar.eventDateFormat + " " + m_config->config().calendar.eventTimeFormat
  );
  content->addChild(headline(i18n::tr("control-center.calendar.next-event", "title", next->title, "when", when),
                             ColorRole::Label2, Tokens::typeFootnote, FontWeight::Medium));
}

void CalendarPage::toggleEventsCard() {
  m_showEventsCard = !m_showEventsCard;
  if (m_config != nullptr) {
    m_config->setOverride({"control_center", "calendar", "show_events_card"}, m_showEventsCard);
  }
  if (m_toggleEventsCardButton != nullptr) {
    m_toggleEventsCardButton->setGlyph(m_showEventsCard ? "calendar-event" : "calendar-off");
    m_toggleEventsCardButton->setSelected(m_showEventsCard);
  }
  if (m_eventsCard != nullptr) {
    m_eventsCard->setVisible(m_showEventsCard);
  }
  if (m_eventsHeader != nullptr) {
    m_eventsHeader->setVisible(m_showEventsCard);
  }
}
