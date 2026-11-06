#include "overview/interact.h"

#include <cmath>
#include <limits>
#include <xkbcommon/xkbcommon.h>

namespace andromeda::ov {

  namespace {
    char lower(char character) {
      return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    bool contains(std::string_view haystack, std::string_view needle) {
      if (needle.size() > haystack.size()) {
        return false;
      }
      for (size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        size_t index = 0;
        while (index < needle.size() && lower(haystack[start + index]) == lower(needle[index])) {
          ++index;
        }
        if (index == needle.size()) {
          return true;
        }
      }
      return false;
    }
  } // namespace

  bool filterMatches(std::string_view filter, std::string_view title, std::string_view appId) {
    return filter.empty() || contains(title, filter) || contains(appId, filter);
  }

  std::string filterText(uint32_t keysym, bool filterEmpty) {
    const uint32_t code = xkb_keysym_to_utf32(keysym);
    if (code < 0x20 || code == 0x7F || (code >= 0x80 && code < 0xA0) || code > 0x10FFFF) {
      return {};
    }
    if (code == ' ' && filterEmpty) {
      return {};
    }
    std::string out;
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    return out;
  }

  void popFilterCharacter(std::string& filter) {
    while (!filter.empty()) {
      const auto byte = static_cast<unsigned char>(filter.back());
      filter.pop_back();
      if ((byte & 0xC0) != 0x80) {
        return;
      }
    }
  }

  int workspaceDigit(uint32_t keysym) {
    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
      return static_cast<int>(keysym - XKB_KEY_1);
    }
    if (keysym >= XKB_KEY_KP_1 && keysym <= XKB_KEY_KP_9) {
      return static_cast<int>(keysym - XKB_KEY_KP_1);
    }
    return -1;
  }

  int pickInDirection(std::span<const Point> candidates, Point from, Direction direction) {
    int best = -1;
    double bestCost = std::numeric_limits<double>::infinity();
    for (size_t index = 0; index < candidates.size(); ++index) {
      const double dx = candidates[index].x - from.x;
      const double dy = candidates[index].y - from.y;
      double along = 0;
      double across = 0;
      switch (direction) {
      case Direction::Left:
        along = -dx;
        across = dy;
        break;
      case Direction::Right:
        along = dx;
        across = dy;
        break;
      case Direction::Up:
        along = -dy;
        across = dx;
        break;
      case Direction::Down:
        along = dy;
        across = dx;
        break;
      }
      if (along <= 0.5) {
        continue;
      }
      const double cost = along + 2 * std::abs(across);
      if (cost < bestCost) {
        bestCost = cost;
        best = static_cast<int>(index);
      }
    }
    return best;
  }

  size_t cycleIndex(size_t count, int current, int step) {
    if (count == 0) {
      return 0;
    }
    if (current < 0 || static_cast<size_t>(current) >= count) {
      return step < 0 ? count - 1 : 0;
    }
    const auto n = static_cast<long>(count);
    return static_cast<size_t>(((current + step) % n + n) % n);
  }

} // namespace andromeda::ov
