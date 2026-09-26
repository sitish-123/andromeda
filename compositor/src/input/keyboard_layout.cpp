#include "input/keyboard_layout.h"

namespace umbriel {
  std::optional<xkb_layout_index_t> layoutGroupNamed(xkb_keymap* keymap, std::string_view name) {
    if (keymap == nullptr || name.empty()) {
      return std::nullopt;
    }
    const xkb_layout_index_t count = xkb_keymap_num_layouts(keymap);
    for (xkb_layout_index_t group = 0; group < count; ++group) {
      const char* candidate = xkb_keymap_layout_get_name(keymap, group);
      if (candidate != nullptr && candidate == name) {
        return group;
      }
    }
    return std::nullopt;
  }

  std::optional<xkb_layout_index_t>
  matchingLayoutGroup(xkb_keymap* target, xkb_keymap* source, xkb_layout_index_t sourceGroup) {
    if (target == nullptr || source == nullptr || sourceGroup >= xkb_keymap_num_layouts(source)) {
      return std::nullopt;
    }
    const char* sourceName = xkb_keymap_layout_get_name(source, sourceGroup);
    if (sourceName == nullptr) {
      return std::nullopt;
    }
    return layoutGroupNamed(target, sourceName);
  }

} // namespace umbriel
