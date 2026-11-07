#pragma once

#include "render/scene/node.h"

#include <cstdint>

class Box;
class Flex;
class Glyph;
class Label;
class Renderer;
struct FileInfo;

struct FileCellState {
  bool selected = false;
  bool hovered = false;
  bool cursor = false;   // keyboard cursor: focus ring
  bool disabled = false; // picker filter rejects it (D12): 38 %
  bool ghost = false;    // hidden file: 56 %
};

// Icons view cell (mockup §1): 96 x 112, icon 64 on a fill_2 plate when selected, name two lines below in an accent
// 24 % pill when selected (D5), middle ellipsis on line 2 (D4).
class FileIconCell final : public Node {
public:
  static constexpr float kWidth = 96.0F;
  static constexpr float kHeight = 112.0F;

  explicit FileIconCell(float scale);
  void bind(Renderer& renderer, const FileInfo& info, float width, float height, FileCellState state);

private:
  float m_scale;
  Box* m_plate = nullptr;
  Glyph* m_icon = nullptr;
  Box* m_namePill = nullptr;
  Label* m_name = nullptr;
  Box* m_ring = nullptr;
};

// List view row (rulebook 9.7, mockup §2): 32 high, symbol 16, Name, Date, Size (tabular, right aligned), Kind.
// Columns drop as the pane narrows: Kind first, then Date (FILES-UX 2.9); Name keeps >= 160.
class FileListRow final : public Node {
public:
  static constexpr float kHeight = 32.0F;
  struct Columns {
    float date = 0.0F; // 0 = hidden
    float size = 0.0F;
    float kind = 0.0F;
  };
  [[nodiscard]] static Columns columnsFor(float width, float scale);

  explicit FileListRow(float scale);
  void bind(Renderer& renderer, const FileInfo& info, float width, FileCellState state);

private:
  float m_scale;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  Glyph* m_icon = nullptr;
  Label* m_name = nullptr;
  Label* m_date = nullptr;
  Label* m_size = nullptr;
  Label* m_kind = nullptr;
};
