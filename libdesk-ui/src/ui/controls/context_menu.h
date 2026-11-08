#pragma once

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/controls/color_swatch_preview.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Renderer;

enum class ContextSubmenuDirection : std::uint8_t {
  Right,
  Left,
};

struct ContextMenuControlEntry {
  std::int32_t id = 0;
  std::string label;
  // Trailing key text in label_3 ("Ctrl+Shift+Z").
  std::string shortcut;
  bool enabled = true;
  bool separator = false;
  // Non-interactive group heading; entries below it read as members of the group.
  bool header = false;
  bool hasSubmenu = false;
  // Rows of a submenu ContextMenuPopup opens itself beside this row (with hasSubmenu).
  std::vector<ContextMenuControlEntry> children;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;
  // Explicit hierarchy depth; each level adds one medium spacing step before row content.
  std::uint8_t indentLevel = 0;
  // Optional leading visual (dropdown-style rows): a small color dot, or a palette swatch strip.
  std::optional<ColorSpec> indicatorColor;
  ColorSwatchPreview swatchPreview;
  TextEllipsize ellipsize = TextEllipsize::End;
  // A leading 16 symbol. When any row has one, every row keeps the symbol column and check/radio marks draw in it.
  std::string glyph;
  bool danger = false; // a destructive action: label and symbol in the danger colour
  // A row of colour dots instead of a label (Files tag row); a click on a dot activates the entry with that id.
  struct Dot {
    std::int32_t id = 0;
    ColorSpec color;
  };
  std::vector<Dot> dots;
};

class ContextMenuControl : public Node {
public:
  ContextMenuControl();

  void setEntries(std::vector<ContextMenuControlEntry> entries);
  void setMaxVisible(std::size_t maxVisible);
  void setMenuWidth(float width);
  void setContentScale(float scale);
  void setReserveToggleSpace(bool reserve);
  void setSubmenuDirection(ContextSubmenuDirection direction);
  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> onActivate);
  void setOnSubmenuOpen(std::function<void(const ContextMenuControlEntry&, float rowCenterY)> onSubmenuOpen);
  void setRedrawCallback(std::function<void()> redrawCallback);

  void setHighlightedIndex(std::size_t index);
  [[nodiscard]] bool moveHighlight(int delta);
  [[nodiscard]] bool activateHighlighted();
  [[nodiscard]] std::size_t highlightedIndex() const noexcept { return m_highlightedIndex; }
  [[nodiscard]] std::size_t entryCount() const noexcept { return m_entries.size(); }
  [[nodiscard]] float rowTop(std::size_t index) const noexcept;
  [[nodiscard]] float rowBottom(std::size_t index) const noexcept;

  [[nodiscard]] float preferredHeight() const;
  [[nodiscard]] static float
  preferredHeight(const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible, float scale = 1.0F);
  // Width that fits the widest entry without elision (label + toggle/submenu slots + padding).
  [[nodiscard]] static float preferredWidth(
      Renderer& renderer, const std::vector<ContextMenuControlEntry>& entries, float scale = 1.0F,
      bool reserveToggleSpace = false
  );

private:
  struct RowVisual {
    std::function<void(bool)> apply;
    float y = 0.0F;
    float height = 0.0F;
    bool interactive = false;
  };

  void doLayout(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void rebuildRows(Renderer& renderer);
  void applyHighlightVisuals();
  [[nodiscard]] std::size_t firstInteractiveIndex() const noexcept;

  std::vector<ContextMenuControlEntry> m_entries;
  std::vector<RowVisual> m_rows;
  std::size_t m_maxVisible = 14;
  std::size_t m_highlightedIndex = 0;
  float m_menuWidth = 246.0F;
  float m_contentScale = 1.0F;
  ContextSubmenuDirection m_submenuDirection = ContextSubmenuDirection::Right;
  bool m_needsRebuild = true;
  bool m_reserveToggleSpace = false;
  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void(const ContextMenuControlEntry&, float rowCenterY)> m_onSubmenuOpen;
  std::function<void()> m_redrawCallback;
};
