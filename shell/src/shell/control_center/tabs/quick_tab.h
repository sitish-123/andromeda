#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class Button;
class Flex;
class Glyph;
class Image;
class Label;
class ScrollView;
class Slider;

// The control center's first view: a compact sheet. The top toggle block pages horizontally (reference block, mode
// toggles, record and microphone); brightness, volume, media, phone and a battery and power-profile footer stay fixed.
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
    std::string glyph; // replaces the shortcut icon when set
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  std::unique_ptr<Flex> makeCard();
  // An empty `detailTab` makes the card toggle; with `chevron` the card toggles and a chevron opens `detailTab`.
  std::unique_ptr<Button> makeToggleRow(
      ToggleRow& row, std::string_view detailTab, float iconTextGap, bool chevron = false
  );
  // A small toggle card for the shortcut `type`, kept in m_extras; null when the type has no shortcut.
  std::unique_ptr<Flex> makeExtraCard(std::string_view type, std::string_view detailTab, bool chevron);
  std::unique_ptr<Flex> makeDots(std::size_t pages);
  std::unique_ptr<Flex> makeFooter();
  // Sizes every page to the sheet width and to the height of page 1.
  void sizePages(Renderer& renderer, float width);
  void syncPager();
  void refreshDots(float offset);
  std::unique_ptr<Flex> makePill(std::string_view glyph, Slider*& slider, Label*& value);
  void syncToggleRow(ToggleRow& row, const std::string& subtitle);
  void syncSliders();
  void syncExtras();
  void syncFooter();
  void syncMedia(Renderer& renderer);
  void syncPhone();

  ControlCenterServices m_services;
  OpenTabCallback m_openTab;
  Flex* m_root = nullptr;

  ToggleRow m_wifi;
  ToggleRow m_bluetooth;
  ToggleRow m_focus;
  ToggleRow m_darkMode;

  // Mode toggles on the pages after the reference block.
  std::vector<ToggleRow> m_extras;
  ScrollView* m_pager = nullptr;
  std::vector<Flex*> m_pages; // page 1 first
  std::vector<Flex*> m_dots;
  Flex* m_dotsRow = nullptr;
  float m_pageWidth = 0.0F;
  std::size_t m_page = 0;
  std::chrono::steady_clock::time_point m_lastScrollAt;

  Flex* m_footer = nullptr;
  Flex* m_batteryBox = nullptr;
  Glyph* m_batteryGlyph = nullptr;
  Label* m_batteryText = nullptr;
  Button* m_profileBox = nullptr;
  Glyph* m_profileGlyph = nullptr;
  Label* m_profileText = nullptr;
  std::unique_ptr<Shortcut> m_profile;

  bool m_syncingSliders = false;
  Flex* m_brightnessPill = nullptr;
  Slider* m_brightnessSlider = nullptr;
  Label* m_brightnessValue = nullptr;
  Slider* m_volumeSlider = nullptr;
  Label* m_volumeValue = nullptr;
  Flex* m_micPill = nullptr;
  Slider* m_micSlider = nullptr;
  Label* m_micValue = nullptr;

  Flex* m_mediaCard = nullptr;
  Label* m_mediaTitle = nullptr;
  Label* m_mediaArtist = nullptr;
  Image* m_mediaArt = nullptr;
  std::string m_mediaArtUrl;
  std::unordered_set<std::string> m_pendingArt;
  std::shared_ptr<int> m_aliveGuard = std::make_shared<int>(0);
  Button* m_playButton = nullptr;

  Flex* m_phoneCard = nullptr;
  Label* m_phoneName = nullptr;
  Label* m_phoneStatus = nullptr;
  std::string m_phoneId;
  std::unique_ptr<Shortcut> m_phoneRing;
};
