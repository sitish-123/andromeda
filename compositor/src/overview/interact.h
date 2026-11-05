#pragma once

#include "overview/style.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Keyboard selection and type-to-filter for the overview. Pure: the controller feeds it card centres and window text.
namespace andromeda::ov {

  // Whether `filter` occurs in the window's title or app id, ignoring ASCII case. An empty filter matches every window.
  [[nodiscard]] bool filterMatches(std::string_view filter, std::string_view title, std::string_view appId);

  // The UTF-8 text a keysym adds to the filter; empty for control and function keys. A leading space is not text.
  [[nodiscard]] std::string filterText(uint32_t keysym, bool filterEmpty);

  // `filter` without its last UTF-8 character.
  void popFilterCharacter(std::string& filter);

  // The workspace a digit key jumps to, 0-based (1 -> 0, ..., 9 -> 8); -1 for any other key.
  [[nodiscard]] int workspaceDigit(uint32_t keysym);

  enum class Direction : uint8_t { Left, Right, Up, Down };

  // The candidate nearest to `from` that lies strictly in `direction`, weighing sideways offset twice (along + 2 *
  // |across|); -1 when none does.
  [[nodiscard]] int pickInDirection(std::span<const Point> candidates, Point from, Direction direction);

  // The index `step` places (+1 next, -1 previous) after `current` among `count` entries, wrapping; with no current
  // entry (-1) the first or last.
  [[nodiscard]] size_t cycleIndex(size_t count, int current, int step);

} // namespace andromeda::ov
