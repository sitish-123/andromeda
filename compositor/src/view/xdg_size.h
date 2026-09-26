#pragma once

#include "wlr.h"

#include <algorithm>

namespace umbriel {

  struct XdgSizeHints {
    int minWidth = 1;
    int minHeight = 1;
    int maxWidth = 0;  // 0 = unlimited
    int maxHeight = 0; // 0 = unlimited
  };

  [[nodiscard]] inline XdgSizeHints xdgSizeHints(const wlr_xdg_toplevel* toplevel) {
    XdgSizeHints hints;
    if (toplevel == nullptr) {
      return hints;
    }
    const auto& state = toplevel->current;
    if (state.min_width > 0) {
      hints.minWidth = state.min_width;
    }
    if (state.min_height > 0) {
      hints.minHeight = state.min_height;
    }
    if (state.max_width > 0) {
      hints.maxWidth = state.max_width;
    }
    if (state.max_height > 0) {
      hints.maxHeight = state.max_height;
    }
    if (hints.maxWidth > 0 && hints.maxWidth < hints.minWidth) {
      hints.maxWidth = hints.minWidth;
    }
    if (hints.maxHeight > 0 && hints.maxHeight < hints.minHeight) {
      hints.maxHeight = hints.minHeight;
    }
    return hints;
  }

  [[nodiscard]] inline int clampXdgWidth(int width, const XdgSizeHints& hints) {
    width = std::max(width, hints.minWidth);
    if (hints.maxWidth > 0) {
      width = std::min(width, hints.maxWidth);
    }
    return std::max(1, width);
  }

  [[nodiscard]] inline int clampXdgHeight(int height, const XdgSizeHints& hints) {
    height = std::max(height, hints.minHeight);
    if (hints.maxHeight > 0) {
      height = std::min(height, hints.maxHeight);
    }
    return std::max(1, height);
  }

} // namespace umbriel
