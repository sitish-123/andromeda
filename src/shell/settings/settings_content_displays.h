#pragma once

#include "wayland/mirror_control.h"
#include "wayland/output_management.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

class Flex;

namespace settings {

  struct SettingsDisplaysContext {
    float scale = 1.0F;
    const OutputManagement* outputs = nullptr;
    // Null when the compositor has no dsk_output_manager_v1.
    const MirrorControl* mirrors = nullptr;
    // Desired state, one entry per head, in head order.
    std::span<const OutputHeadConfig> edits;
    bool dirty = false;
    int confirmSecondsLeft = 0; // > 0 while a just-applied change awaits Keep or Revert
    std::string error;
    std::function<void(OutputHeadConfig)> edit;
    std::function<void()> apply;
    std::function<void()> discard;
    std::function<void()> keep;
    std::function<void()> revert;
    // An empty source stops mirroring.
    std::function<void(std::string target, std::string source)> setMirror;
  };

  void addSettingsDisplays(Flex& content, const SettingsDisplaysContext& ctx);

  enum class DisplayPlacement : std::uint8_t {
    RightOf,
    LeftOf,
    Above,
    Below,
  };

  // Size in layout coordinates: mode size divided by scale, with width and height swapped for 90 and 270 degree
  // transforms.
  [[nodiscard]] std::pair<int, int> displayLogicalSize(const OutputHead& head, const OutputHeadConfig& config);

  // Top-left position that puts a display of `size` against the given edge of `anchor`, aligned to its top or left.
  [[nodiscard]] std::pair<int, int> displayPlacementPosition(
      const OutputHeadConfig& anchor, std::pair<int, int> anchorSize, std::pair<int, int> size,
      DisplayPlacement placement
  );

} // namespace settings
