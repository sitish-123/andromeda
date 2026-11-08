#pragma once

#include "render/scene/node.h"

#include <cstdint>
#include <filesystem>
#include <utility>

class Box;
class Flex;
class FileIcon;
class Glyph;
class Label;
class Renderer;
class ThumbnailService;
struct FileInfo;

struct FileCellState {
  bool selected = false;
  bool hovered = false;
  bool cursor = false;   // keyboard cursor: focus ring
  bool disabled = false; // picker filter rejects it (D12): 38 %
  bool ghost = false;    // hidden file: 56 %
  bool dropTarget = false; // a drag hovers this folder: accent ring + tint (mockup §22)
  bool inactive = false;   // selection in a column that is not the current one: grey fill_2 (mockup §22)
  bool nameOnly = false;   // columns view: icon and name, no Date/Size/Kind, a chevron on folders
  float pressScale = 1.0F; // icons view: the pressed cell runs 1 -> 0.98 on spring.press
  float iconSize = 0.0F;   // icons view icon in logical px (zoom 48-128); 0 = component.grid.icon
  int depth = 0;           // list view: expanded-in-place level, indented 16 per level
  enum class Disclosure : std::uint8_t { None, Leaf, Collapsed, Expanded } disclosure = Disclosure::None; // list view
  std::filesystem::path path; // the item's full path: folder contents and user-folder emblems in its icon
  ThumbnailService* thumbnails = nullptr; // image, video and PDF previews; null = drawn art only
};

// Icons view cell (mockup §1, .icw): 96 x 112 at icon 64; a 72 x 68 plate 4 from the top (hover fill_1, press fill_2,
// selected fill_2), name 500 12/16 two lines 4 below in an accent pill when selected (fill_2 when inactive), middle
// ellipsis on line 2 (D4). Cell size, plate and label width follow the icon size.
class FileIconCell final : public Node {
public:

  // Cell size for an icon size in logical px (0 = component.grid.icon), scaled.
  [[nodiscard]] static float cellWidth(float iconSize, float scale);
  [[nodiscard]] static float cellHeight(float iconSize, float scale);

  explicit FileIconCell(float scale);
  void bind(Renderer& renderer, const FileInfo& info, float width, float height, FileCellState state);
  [[nodiscard]] Node* nameNode() const noexcept; // the inline rename field covers it

private:
  float m_scale;
  Box* m_plate = nullptr;
  FileIcon* m_icon = nullptr;
  float m_iconSize = 0.0F; // logical px of m_icon
  Box* m_namePill = nullptr;
  Label* m_name = nullptr;
  Box* m_ring = nullptr;
};

// List view row (rulebook 9.7, mockup .trow): 32 high r8, 8 from the pane edge (4 in the columns view), padding 0 8,
// icon 20, Name, Date 112, Size 72 (tabular, right aligned), Kind 112 with 16 before it; 400 13/16.
// Columns drop as the pane narrows: Kind first, then Date (FILES-UX 2.9); Name keeps >= 120.
class FileListRow final : public Node {
public:
  struct Columns {
    float date = 0.0F; // 0 = hidden
    float size = 0.0F;
    float kind = 0.0F;
  };
  [[nodiscard]] static Columns columnsFor(float width, float scale);
  // Space between the pane edge and the row's text: row inset + row padding (the header uses it too).
  [[nodiscard]] static float textInset(float scale);
  // The disclosure triangle's x range in a row of this depth (press there toggles the folder).
  [[nodiscard]] static std::pair<float, float> disclosureSpan(int depth, float scale);

  explicit FileListRow(float scale);
  void bind(Renderer& renderer, const FileInfo& info, float width, FileCellState state);
  [[nodiscard]] Node* nameNode() const noexcept;

private:
  float m_scale;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  Flex* m_indent = nullptr;
  Glyph* m_disclosure = nullptr;
  FileIcon* m_icon = nullptr;
  Label* m_name = nullptr;
  Label* m_date = nullptr;
  Label* m_size = nullptr;
  Flex* m_kindBox = nullptr;
  Label* m_kind = nullptr;
  Glyph* m_chevron = nullptr;
};
