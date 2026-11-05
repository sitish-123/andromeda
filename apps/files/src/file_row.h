#pragma once

#include "render/scene/input_area.h"

#include <cstdint>
#include <functional>
#include <string>

class Box;
class Flex;
class Glyph;
class Label;
class Renderer;
struct FileInfo;

// One row of the list view (rulebook §9.7: 32 high, symbol 16, Name / Size / Kind / Modified).
class FileRow final : public InputArea {
public:
  struct Callbacks {
    std::function<void(std::size_t index, std::uint32_t modifiers)> select;
    std::function<void(std::size_t index)> open;
    std::function<void(std::size_t index, float sceneX, float sceneY)> menu;
  };

  explicit FileRow(float scale, Callbacks callbacks);

  void bind(Renderer& renderer, const FileInfo& info, std::size_t index, float width, bool selected, bool hovered);
  [[nodiscard]] static float height(float scale) noexcept;
  // The keyboard modifiers held at the last press, set by the window from the seat.
  static void setModifiers(std::uint32_t modifiers) noexcept;

private:
  float m_scale;
  Callbacks m_callbacks;
  std::size_t m_index = 0;
  std::uint32_t m_lastClickMs = 0;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  Glyph* m_icon = nullptr;
  Label* m_name = nullptr;
  Label* m_size = nullptr;
  Label* m_kind = nullptr;
  Label* m_date = nullptr;
};

[[nodiscard]] std::string formatSize(std::uint64_t bytes);
[[nodiscard]] std::string formatModified(std::int64_t mtimeNs);
// Tabler glyph for a content type ("folder", "photo", "file-text", ...).
[[nodiscard]] const char* glyphForType(const FileInfo& info);
