#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Button;
class Flex;
class Glyph;
class Label;
class Slider;

// The control center's first view: a compact sheet of cards (connectivity, Focus, Dark Mode, brightness, volume,
// media, phone). Each card opens its full tab as the detail view.
class QuickTab : public Tab {
public:
  using OpenTabCallback = std::function<void(std::string_view tabKey)>;

  QuickTab(const ControlCenterServices& services, OpenTabCallback openTab);

  std::unique_ptr<Flex> create() override;
  // Natural height of the sheet at `width`.
  [[nodiscard]] float measureHeight(Renderer& renderer, float width);

private:
  // A row of the connectivity card or a small card: round toggle icon, title, subtitle.
  struct ToggleRow {
    std::unique_ptr<Shortcut> shortcut;
    Button* icon = nullptr;
    Label* title = nullptr;
    Label* subtitle = nullptr;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  std::unique_ptr<Flex> makeCard();
  std::unique_ptr<Button> makeToggleRow(ToggleRow& row, std::string_view detailTab);
  std::unique_ptr<Flex> makePill(std::string_view glyph, Slider*& slider, Label*& value);
  void syncToggleRow(ToggleRow& row, const std::string& subtitle);
  void syncSliders();
  void syncMedia();
  void syncPhone();

  ControlCenterServices m_services;
  OpenTabCallback m_openTab;
  Flex* m_root = nullptr;

  ToggleRow m_wifi;
  ToggleRow m_bluetooth;
  ToggleRow m_focus;
  ToggleRow m_darkMode;

  Flex* m_brightnessPill = nullptr;
  Slider* m_brightnessSlider = nullptr;
  Label* m_brightnessValue = nullptr;
  Slider* m_volumeSlider = nullptr;
  Label* m_volumeValue = nullptr;
  bool m_draggingSlider = false;

  Flex* m_mediaCard = nullptr;
  Label* m_mediaTitle = nullptr;
  Label* m_mediaArtist = nullptr;
  Button* m_playButton = nullptr;

  Flex* m_phoneCard = nullptr;
  Label* m_phoneName = nullptr;
  Label* m_phoneStatus = nullptr;
  std::unique_ptr<Shortcut> m_phoneRing;
};
