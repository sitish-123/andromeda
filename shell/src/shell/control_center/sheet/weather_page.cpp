#include "shell/control_center/sheet/weather_page.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace {

  constexpr std::size_t kHourCount = 24;
  constexpr std::size_t kDayCount = 7;
  constexpr float kHourCellWidth = 48.0F;
  constexpr float kHourStripHeight = 92.0F;
  constexpr float kDayLabelWidth = 56.0F;
  constexpr float kTempLabelWidth = 36.0F;
  constexpr float kRangeBarHeight = 4.0F;
  // Below this rain chance an hour shows no percent (iOS shows it from 10 %).
  constexpr int kRainShowPercent = 10;

  std::string weekdayShort(const std::string& isoDate) {
    if (isoDate.size() != 10) {
      return isoDate;
    }
    std::tm tm{};
    tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(isoDate.substr(8, 2));
    tm.tm_hour = 12;
    if (std::mktime(&tm) == -1) {
      return isoDate;
    }
    return formatStrftime("%a", tm);
  }

  std::string compass(int degrees) {
    static constexpr std::array<const char*, 8> kPoints{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int index = static_cast<int>(std::lround(((degrees % 360) + 360) % 360 / 45.0)) % 8;
    return kPoints[static_cast<std::size_t>(index)];
  }

  std::string uvLevelKey(double uv) {
    if (uv < 3.0) {
      return "control-center.weather.uv.low";
    }
    if (uv < 6.0) {
      return "control-center.weather.uv.moderate";
    }
    if (uv < 8.0) {
      return "control-center.weather.uv.high";
    }
    if (uv < 11.0) {
      return "control-center.weather.uv.very-high";
    }
    return "control-center.weather.uv.extreme";
  }

  std::unique_ptr<Label> text(
      std::string value, const Tokens::Type& type, ColorRole role, float scale, FontWeight weight = FontWeight::Medium
  ) {
    auto label = ui::label({
        .text = std::move(value),
        .fontSize = type.size * scale,
        .fontWeight = weight,
        .color = colorSpecFromRole(role),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    });
    label->setLineHeight(type.line * scale);
    return label;
  }

} // namespace

WeatherPage::WeatherPage(WeatherService* weather, ConfigService* config) : m_weather(weather), m_config(config) {}

int WeatherPage::temperature(double celsius) const {
  return static_cast<int>(std::lround(m_weather != nullptr ? m_weather->displayTemperature(celsius) : celsius));
}

std::string WeatherPage::timeFormat() const {
  const std::string fmt = m_config != nullptr ? m_config->config().shell.timeFormat : std::string{};
  return fmt.empty() ? std::string("%H:%M") : fmt;
}

std::unique_ptr<Flex> WeatherPage::create() {
  const float scale = contentScale();
  if (m_weather != nullptr && !m_changeCallbackRegistered) {
    m_changeCallbackRegistered = true;
    m_weather->addChangeCallback([] { PanelManager::instance().refresh(); });
  }

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Tokens::componentSheetGap * scale,
  });
  // The sheet's own header: back to the clock calendar the page opens from, and the place.
  tab->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .height = Tokens::componentSheetDetailHeader * scale},
      ui::button({
          .glyph = "chevron-left",
          .glyphSize = Tokens::sizeSymbol * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.weather.back"),
          .minHeight = Tokens::sizeControlM * scale,
          .paddingH = Tokens::spaceP8 * scale,
          .radius = Tokens::radiusR8 * scale,
          .onClick =
              [] { PanelManager::instance().openPanel("control-center", PanelOpenRequest{.context = "calendar"}); },
      }),
      ui::label({
          .out = &m_title,
          .text = i18n::tr("control-center.tabs.weather"),
          .fontSize = Tokens::typeTitle3.size * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  ));
  // The body scrolls when the screen is shorter than the page; the header stays.
  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .contentScale = scale,
      .scrollbarAutoHide = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  m_body = scroll->content();
  m_body->setAlign(FlexAlign::Stretch);
  m_body->setGap(Tokens::componentSheetGap * scale);
  tab->addChild(std::move(scroll));
  m_shownFetchedAt = {};
  m_shownState.clear();
  sync();
  return tab;
}

void WeatherPage::onClose() {
  m_rootLayout = nullptr;
  m_body = nullptr;
  m_scroll = nullptr;
  m_title = nullptr;
  m_hourly = nullptr;
  m_shownState.clear();
}

void WeatherPage::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  sync();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void WeatherPage::doUpdate(Renderer& renderer) {
  (void)renderer;
  sync();
}

float WeatherPage::measureHeight(Renderer& renderer, float width) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  sync();
  // Natural height: the header row, the gap and the whole body, so the sheet grows to fit before it scrolls.
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  const Node* header = m_rootLayout->children().front().get();
  return header->height() + m_rootLayout->gap() + m_body->measure(renderer, constraints).height;
}

void WeatherPage::sync() {
  if (m_body == nullptr) {
    return;
  }
  std::string state;
  if (m_weather == nullptr || !m_weather->enabled()) {
    state = "off";
  } else if (m_weather->hasData()) {
    state = "data";
  } else if (!m_weather->locationConfigured()) {
    state = "no-location";
  } else {
    state = m_weather->error().empty() ? "loading" : "error";
  }
  const bool sameData = state != "data" || m_weather->snapshot().fetchedAt == m_shownFetchedAt;
  if (state == m_shownState && sameData) {
    return;
  }
  m_shownState = state;
  while (!m_body->children().empty()) {
    m_body->removeChild(m_body->children().back().get());
  }
  m_hourly = nullptr;
  if (state == "data") {
    m_shownFetchedAt = m_weather->snapshot().fetchedAt;
    buildBody(m_weather->snapshot());
  } else if (state == "off") {
    buildMessage("weather-cloud-off", i18n::tr("control-center.weather.off-title"),
                 i18n::tr("control-center.weather.off-body"), false);
  } else if (state == "no-location") {
    buildMessage("map-pin-off", i18n::tr("control-center.weather.no-location-title"),
                 i18n::tr("control-center.weather.no-location-body"), true);
  } else if (state == "error") {
    buildMessage("cloud-off", i18n::tr("control-center.weather.data-unavailable"), m_weather->error(), false);
  } else {
    buildMessage("weather-cloud-sun", i18n::tr("control-center.weather.fetching"), {}, false);
  }
  PanelManager::instance().refresh();
}

std::unique_ptr<Flex> WeatherPage::makeCard(float gap) const {
  const float scale = contentScale();
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = gap,
      .padding = Tokens::componentTilePadding * scale,
      .fill = colorSpecFromRole(ColorRole::GlassCard),
      .radius = Tokens::componentTileRadius * scale,
  });
  card->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * scale);
  return card;
}

void WeatherPage::buildMessage(
    const std::string& glyph, const std::string& title, const std::string& body, bool settingsAction
) {
  const float scale = contentScale();
  auto card = makeCard(Tokens::spaceP4 * scale);
  card->setAlign(FlexAlign::Center);
  card->setPadding(Tokens::spaceP16 * scale);
  card->addChild(ui::glyph({
      .glyph = glyph,
      .glyphSize = Tokens::sizeControlM * scale,
      .color = colorSpecFromRole(ColorRole::Label2),
  }));
  card->addChild(text(title, Tokens::typeHeadline, ColorRole::Label1, scale, FontWeight::SemiBold));
  if (!body.empty()) {
    auto bodyLabel = text(body, Tokens::typeFootnote, ColorRole::Label2, scale);
    bodyLabel->setMaxLines(3);
    bodyLabel->setTextAlign(TextAlign::Center);
    card->addChild(std::move(bodyLabel));
  }
  if (settingsAction) {
    card->addChild(ui::button({
        .text = i18n::tr("control-center.weather.set-location"),
        .fontSize = Tokens::typeFootnote.size * scale,
        .variant = ButtonVariant::Default,
        .minHeight = Tokens::sizeControlS * scale,
        .paddingH = Tokens::spaceP8 * scale,
        .radius = Tokens::radiusR8 * scale,
        .onClick = [] { PanelManager::instance().openSettingsWindow("location"); },
    }));
  }
  if (m_title != nullptr) {
    m_title->setText(i18n::tr("control-center.tabs.weather"));
  }
  m_body->addChild(std::move(card));
}

void WeatherPage::buildBody(const WeatherSnapshot& snap) {
  const float scale = contentScale();
  if (m_title != nullptr) {
    m_title->setText(snap.locationName.empty() ? i18n::tr("control-center.tabs.weather") : snap.locationName);
  }

  // Hero: large temperature, condition, today's high and low.
  auto hero = ui::column({.align = FlexAlign::Center, .paddingV = Tokens::spaceP4 * scale});
  auto temp = text(std::format("{}°", temperature(snap.current.temperatureC)), Tokens::typeHero, ColorRole::Label1,
                   scale, FontWeight::Light);
  hero->addChild(std::move(temp));
  hero->addChild(text(WeatherService::descriptionForCode(snap.current.weatherCode), Tokens::typeHeadline,
                      ColorRole::Label2, scale, FontWeight::SemiBold));
  if (!snap.forecastDays.empty()) {
    hero->addChild(text(
        i18n::tr("control-center.calendar.weather-high-low", "high",
                 std::to_string(temperature(snap.forecastDays.front().temperatureMaxC)), "low",
                 std::to_string(temperature(snap.forecastDays.front().temperatureMinC))),
        Tokens::typeFootnote, ColorRole::Label2, scale
    ));
  }
  m_body->addChild(std::move(hero));
  m_body->addChild(makeHourly(snap));
  m_body->addChild(makeDaily(snap));
  m_body->addChild(makeTiles(snap));
}

std::unique_ptr<Flex> WeatherPage::makeHourly(const WeatherSnapshot& snap) {
  const float scale = contentScale();
  auto card = makeCard(0.0F);
  card->setPadding(Tokens::spaceP8 * scale, Tokens::spaceP4 * scale);
  auto scroll = ui::scrollView({
      .out = &m_hourly,
      .contentScale = scale,
      .orientation = ScrollOrientation::Horizontal,
      .scrollbarVisible = false,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .fillWidth = true,
      .height = kHourStripHeight * scale,
  });
  Flex* strip = scroll->content();
  strip->setDirection(FlexDirection::Horizontal);
  strip->setAlign(FlexAlign::Stretch);

  // First hour at or after the current one; the current hour reads "Now" with the live reading.
  const std::string nowHour = snap.current.timeIso.substr(0, 13);
  const auto first = std::ranges::find_if(snap.forecastHours, [&](const WeatherForecastHour& hour) {
    return hour.timeIso.substr(0, 13) >= nowHour;
  });
  const std::string fmt = timeFormat();
  const bool twelveHour = fmt.find("%I") != std::string::npos || fmt.find("%l") != std::string::npos;
  std::size_t count = 0;
  for (auto it = first; it != snap.forecastHours.end() && count < kHourCount; ++it, ++count) {
    const bool now = count == 0;
    const std::string label = now ? i18n::tr("control-center.weather.now")
                                  : formatIsoTime(it->timeIso, twelveHour ? "%-I %p" : "%H");
    const int degrees = temperature(now ? snap.current.temperatureC : it->temperatureC);
    const std::int32_t code = now ? snap.current.weatherCode : it->weatherCode;
    const bool isDay = now ? snap.current.isDay : it->isDay;
    auto cell = ui::column(
        {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP4 * scale,
         .width = kHourCellWidth * scale},
        text(label, Tokens::typeCaption, ColorRole::Label2, scale, FontWeight::SemiBold),
        ui::glyph({
            .glyph = std::string(WeatherService::glyphForCode(code, isDay)),
            .glyphSize = Tokens::sizeSymbolL * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
        }),
        text(std::format("{}°", degrees), Tokens::typeHeadline, ColorRole::Label1, scale, FontWeight::SemiBold)
    );
    // Rain chance only when it matters; an empty line keeps every cell the same height.
    const int rain = it->precipitationProbabilityPercent;
    cell->addChild(text(rain >= kRainShowPercent ? std::format("{} %", rain) : std::string(" "), Tokens::typeCaption,
                        ColorRole::AccentText, scale, FontWeight::SemiBold));
    strip->addChild(std::move(cell));
  }
  card->addChild(std::move(scroll));
  return card;
}

std::unique_ptr<Flex> WeatherPage::makeDaily(const WeatherSnapshot& snap) const {
  const float scale = contentScale();
  auto card = makeCard(0.0F);
  card->setPadding(Tokens::spaceP4 * scale, Tokens::componentTilePadding * scale);
  const std::size_t days = std::min(kDayCount, snap.forecastDays.size());
  if (days == 0) {
    return card;
  }
  double lowest = snap.forecastDays.front().temperatureMinC;
  double highest = snap.forecastDays.front().temperatureMaxC;
  for (std::size_t i = 0; i < days; ++i) {
    lowest = std::min(lowest, snap.forecastDays[i].temperatureMinC);
    highest = std::max(highest, snap.forecastDays[i].temperatureMaxC);
  }
  // Flex grow shares need a positive span; a flat week still draws a full bar.
  const double span = std::max(highest - lowest, 0.1);
  for (std::size_t i = 0; i < days; ++i) {
    const WeatherForecastDay& day = snap.forecastDays[i];
    auto low = text(std::format("{}°", temperature(day.temperatureMinC)), Tokens::typeHeadline, ColorRole::Label2,
                    scale, FontWeight::SemiBold);
    low->setMinWidth(kTempLabelWidth * scale);
    low->setTextAlign(TextAlign::End);
    auto high = text(std::format("{}°", temperature(day.temperatureMaxC)), Tokens::typeHeadline, ColorRole::Label1,
                     scale, FontWeight::SemiBold);
    high->setMinWidth(kTempLabelWidth * scale);
    high->setTextAlign(TextAlign::End);
    auto dayLabel = text(i == 0 ? i18n::tr("control-center.calendar.today") : weekdayShort(day.dateIso),
                         Tokens::typeHeadline, ColorRole::Label1, scale, FontWeight::SemiBold);
    dayLabel->setMinWidth(kDayLabelWidth * scale);

    // Range bar on the week's scale: before | this day's low..high | after.
    auto bar = ui::row({
        .fill = colorSpecFromRole(ColorRole::Fill2),
        .radius = kRangeBarHeight * 0.5F * scale,
        .height = kRangeBarHeight * scale,
        .flexGrow = 1.0F,
    });
    bar->addChild(ui::box({.flexGrow = static_cast<float>((day.temperatureMinC - lowest) / span)}));
    bar->addChild(ui::box({
        .fill = colorSpecFromRole(ColorRole::Accent),
        .radius = kRangeBarHeight * 0.5F * scale,
        .height = kRangeBarHeight * scale,
        .flexGrow = static_cast<float>(std::max(day.temperatureMaxC - day.temperatureMinC, 0.1) / span),
    }));
    bar->addChild(ui::box({.flexGrow = static_cast<float>((highest - day.temperatureMaxC) / span)}));

    auto row = ui::row(
        {.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .height = Tokens::sizeRow * scale},
        std::move(dayLabel),
        ui::glyph({
            .glyph = std::string(WeatherService::glyphForCode(day.weatherCode, true)),
            .glyphSize = Tokens::sizeSymbolL * scale,
            .color = colorSpecFromRole(ColorRole::Label1),
        }),
        std::move(low), std::move(bar), std::move(high)
    );
    card->addChild(std::move(row));
  }
  return card;
}

std::unique_ptr<Flex> WeatherPage::makeTile(
    const std::string& glyph, const std::string& title, const std::string& value, const std::string& detail
) const {
  const float scale = contentScale();
  auto tile = makeCard(Tokens::spaceP4 * scale);
  tile->setFlexGrow(1.0F);
  tile->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale},
      ui::glyph({.glyph = glyph, .glyphSize = Tokens::sizeSymbolInline * scale, .color = colorSpecFromRole(ColorRole::Label2)}),
      text(title, Tokens::typeCaption, ColorRole::Label2, scale, FontWeight::SemiBold)
  ));
  tile->addChild(text(value, Tokens::typeTitle2, ColorRole::Label1, scale, FontWeight::SemiBold));
  tile->addChild(text(detail, Tokens::typeFootnote, ColorRole::Label2, scale));
  return tile;
}

std::unique_ptr<Flex> WeatherPage::makeTiles(const WeatherSnapshot& snap) const {
  const float scale = contentScale();
  const auto& now = snap.current;
  const bool imperial = m_weather != nullptr && m_weather->useImperial();
  const int wind = static_cast<int>(std::lround(imperial ? now.windSpeedKmh * 0.621371 : now.windSpeedKmh));
  const std::string fmt = timeFormat();
  std::string sunrise = "—";
  std::string sunset = "—";
  if (!snap.forecastDays.empty()) {
    if (!snap.forecastDays.front().sunriseIso.empty()) {
      sunrise = formatIsoTime(snap.forecastDays.front().sunriseIso, fmt.c_str());
    }
    if (!snap.forecastDays.front().sunsetIso.empty()) {
      sunset = formatIsoTime(snap.forecastDays.front().sunsetIso, fmt.c_str());
    }
  }

  auto grid = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::componentSheetGap * scale});
  auto rowA = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::componentSheetGap * scale});
  rowA->addChild(makeTile("sun", i18n::tr("control-center.weather.details.uv-index"),
                          std::to_string(static_cast<int>(std::lround(now.uvIndex))), i18n::tr(uvLevelKey(now.uvIndex))));
  rowA->addChild(makeTile("wind", i18n::tr("control-center.weather.details.wind"),
                          std::format("{} {}", wind, imperial ? "mph" : "km/h"),
                          i18n::tr("control-center.weather.wind-from", "direction", compass(now.windDirectionDeg))));
  auto rowB = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::componentSheetGap * scale});
  rowB->addChild(makeTile("droplet", i18n::tr("control-center.weather.details.humidity"),
                          std::format("{} %", now.relativeHumidityPercent),
                          now.apparentTemperatureC.has_value()
                              ? i18n::tr("control-center.weather.feels", "temp",
                                         std::to_string(temperature(*now.apparentTemperatureC)))
                              : std::string(" ")));
  rowB->addChild(makeTile("sunrise", i18n::tr("control-center.weather.details.sunrise"), sunrise,
                          i18n::tr("control-center.weather.sunset-at", "time", sunset)));
  grid->addChild(std::move(rowA));
  grid->addChild(std::move(rowB));
  return grid;
}
