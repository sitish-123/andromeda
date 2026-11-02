#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_registry.h"
#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class Box;
class Button;
class Flex;
class Glyph;
class Image;
class InputArea;
class Node;
class Label;
class ScrollView;
class Slider;
class Spinner;

// The control center's first view: a compact sheet. The top toggle block pages horizontally (reference block, mode
// toggles, record and microphone); brightness, volume, media, phone and a battery and power-profile footer stay fixed.
class QuickTab : public Tab {
public:
  using OpenTabCallback = std::function<void(std::string_view tabKey)>;

  QuickTab(const ControlCenterServices& services, OpenTabCallback openTab);

  std::unique_ptr<Flex> create() override;
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  void onClose() override;
  void setActive(bool active) override;
  Label* sharedTitleFor(std::string_view detailKey) override;
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
    std::string id;    // shortcut type; the key stored in [control_center] quick_tiles
    Flex* card = nullptr;
    Button* body = nullptr;    // the tile row (tooltip, right-click)
    std::string detail;        // detail tab key; empty = none
    Button* chevron = nullptr; // hidden while connecting
    Spinner* spinner = nullptr;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  std::unique_ptr<Flex> makeCard(bool badged = false);
  void addSingleRow(Flex& card, std::unique_ptr<Button> row);
  // An empty `detailTab` makes the card toggle; with `chevron` the card toggles and a chevron opens `detailTab`.
  std::unique_ptr<Button> makeToggleRow(
      ToggleRow& row, std::string_view detailTab, float iconTextGap, bool chevron = false
  );
  // A small toggle card for the shortcut `type`, kept in m_extras; null when the type has no shortcut.
  // The toggle row of a tile, registered in m_extras; its card is set by the caller.
  std::unique_ptr<Button> makeTileRow(std::string_view type, bool plain);
  std::unique_ptr<Flex> makeTileCard(std::string_view type, bool plain = false);
  void attachTileDrag(Button& button);
  void tilePress(Button* button, float x, float y, bool pressed);
  void tileMotion(Button* button, float x, float y);
  // Fills the pager with 2 x 2 pages of the tiles in `order`; an unavailable tile is skipped.
  void buildPages(const std::vector<std::string>& order);
  // The stored order (or the default), deduplicated; tileOrder() falls back to the default when none is available.
  [[nodiscard]] std::vector<std::string> savedTileOrder() const;
  [[nodiscard]] std::vector<std::string> tileOrder() const;
  void saveEdit();
  [[nodiscard]] static const std::vector<std::string>& tileOrderDefault();
  std::unique_ptr<Flex> makeDots();
  void fillDots(std::size_t pages);
  void goToPage(std::size_t page);
  [[nodiscard]] bool tileAvailable(std::string_view id) const;
  std::unique_ptr<Flex> makeFooter();
  // Sizes every page to the sheet width and to the height of page 1.
  void sizePages(Renderer& renderer, float width);
  void syncPager();
  void setEditing(bool editing);
  bool moveFocus(int index);
  // Edit-mode motion: scale on `snap`, tile moves on `move`, badges on `fade` (rulebook 9.16).
  void animateScale(Flex& card, float to);
  void animateFlip();
  void fadeIn(Node& node, float delayMs = 0.0F);
  void startStagger();
  bool focusSlider(Slider* slider);
  // Left/Right/Up/Down on a focused slider; false when no slider holds focus.
  bool handleCardKey(std::uint32_t sym, const InputArea* focused);
  bool handleSliderKey(std::uint32_t sym, std::uint32_t modifiers, const InputArea* focused);
  void buildEditParts(Flex& root);
  void rebuildEdit();
  void reflowEditGrid();
  [[nodiscard]] int editSlotAt(float x, float y) const;
  [[nodiscard]] static float offsetIn(const Node* node, const Node* ancestor, bool y);
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

  // Every tile on every page, in order.
  std::vector<ToggleRow> m_extras;
  ScrollView* m_pager = nullptr;
  std::vector<Flex*> m_pages; // page 1 first
  std::vector<Flex*> m_halfWidth; // extra cards and fillers: half a page each
  std::vector<Flex*> m_dots;
  Flex* m_dotsRow = nullptr;
  float m_pageStep = 0.0F; // page width + the gap between pages
  std::size_t m_page = 0;

  Flex* m_footer = nullptr;
  Button* m_editButton = nullptr;
  bool m_editing = false;
  std::vector<Node*> m_normalParts;
  Flex* m_editHeader = nullptr;
  Flex* m_editGrid = nullptr;
  Flex* m_editGallery = nullptr;
  InputArea* m_editArea = nullptr;
  InputArea* m_galleryArea = nullptr;
  std::vector<std::string> m_editOrder;
  std::vector<Flex*> m_editCards;   // cards of m_editOrder, same order
  std::vector<Flex*> m_editRows;
  std::vector<Flex*> m_editHalf; // edit and gallery cells and fillers: half a page each
  std::vector<Flex*> m_galleryHalf;
  bool m_editReflowPending = false;
  bool m_editRebuildPending = false;
  bool m_focusFirstTile = false;
  bool m_staggerPending = false;
  std::vector<std::pair<Flex*, std::pair<float, float>>> m_flipFrom; // tile -> position in the grid before a reflow
  std::array<Timer, 6> m_staggerTimers;
  Button* m_tileSource = nullptr;
  bool m_tileDragging = false;
  bool m_swallowClick = false;
  float m_tilePressX = 0.0F;
  float m_tilePressY = 0.0F;
  std::vector<std::string> m_galleryIds;
  std::vector<Flex*> m_galleryCards;
  int m_dragIndex = -1;
  Box* m_liftShadow = nullptr;
  void syncLiftShadow();
  [[nodiscard]] float editGridMargin() const;
  int m_pressedIndex = -1;
  bool m_pressedBadge = false;
  float m_pressY = 0.0F;
  float m_pressX = 0.0F;
  bool m_dragMoved = false;
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

  Flex* m_mediaCard = nullptr;
  Label* m_mediaTitle = nullptr;
  Label* m_mediaArtist = nullptr;
  Image* m_mediaArt = nullptr;
  std::string m_mediaArtUrl;
  std::unordered_set<std::string> m_pendingArt;
  std::shared_ptr<int> m_aliveGuard = std::make_shared<int>(0);
  Button* m_playButton = nullptr;

  Flex* m_phoneCard = nullptr;
  std::array<Button*, 3> m_mediaKeys{};
  std::array<Button*, 3> m_phoneKeys{}; // ring, mirror, open
  Label* m_phoneName = nullptr;
  Label* m_phoneStatus = nullptr;
  std::string m_phoneId;
  std::unique_ptr<Shortcut> m_phoneRing;
};
