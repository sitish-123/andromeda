#pragma once

#include "shell/control_center/sheet/sheet_page.h"

#include <chrono>
#include <memory>
#include <string>

class ConfigService;
class Flex;
class Label;
class ScrollView;
class WeatherService;
struct WeatherSnapshot;

// Weather page in the control-center sheet (.agent/DESIGN.md "Weather page", direction A): hero, hourly strip,
// 7-day range bars and 2 x 2 detail tiles. The body rebuilds only when a new forecast arrives.
class WeatherPage : public SheetPage {
public:
  WeatherPage(WeatherService* weather, ConfigService* config);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  [[nodiscard]] float measureHeight(Renderer& renderer, float width);

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync();
  void buildBody(const WeatherSnapshot& snap);
  void buildMessage(const std::string& glyph, const std::string& title, const std::string& body, bool settingsAction);
  [[nodiscard]] std::unique_ptr<Flex> makeCard(float gap) const;
  [[nodiscard]] std::unique_ptr<Flex> makeHourly(const WeatherSnapshot& snap);
  [[nodiscard]] std::unique_ptr<Flex> makeDaily(const WeatherSnapshot& snap) const;
  [[nodiscard]] std::unique_ptr<Flex> makeTiles(const WeatherSnapshot& snap) const;
  [[nodiscard]] std::unique_ptr<Flex> makeTile(
      const std::string& glyph, const std::string& title, const std::string& value, const std::string& detail
  ) const;
  [[nodiscard]] int temperature(double celsius) const;
  [[nodiscard]] std::string timeFormat() const;

  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_body = nullptr;
  Label* m_title = nullptr;
  ScrollView* m_hourly = nullptr;
  ScrollView* m_scroll = nullptr;
  std::chrono::system_clock::time_point m_shownFetchedAt{};
  std::string m_shownState;
  bool m_changeCallbackRegistered = false;
};
