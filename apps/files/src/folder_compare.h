#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Folder compare for split view (mockup files-v2-live.html §16, Directory Opus / Beyond Compare). Files with the same
// size and a modification time within 2 s are the same. A folder on both sides is compared to the bottom: it is
// newer on the side that has a file the other lacks or a newer copy of one.
namespace folder_compare {

  enum class Mark : std::uint8_t {
    Same,
    Newer,    // this side's copy is newer, or (a folder) holds newer or extra files
    Older,
    OnlyLeft, // drawn on the left pane
    OnlyRight // drawn on the right pane
  };

  struct Result {
    std::map<std::string, Mark> left; // top-level names
    std::map<std::string, Mark> right;
    std::size_t same = 0;
    std::size_t differ = 0;
    std::size_t onlyLeft = 0;
    std::size_t onlyRight = 0;
    // What a sync copies, relative to each root: items missing on the other side (a missing folder as a whole) and
    // newer copies, at any depth.
    std::vector<std::filesystem::path> sendLeft;
    std::vector<std::filesystem::path> sendRight;
    bool truncated = false; // the walk stopped at kEntryLimit
  };

  inline constexpr std::size_t kEntryLimit = 500'000;

  // Walks both trees: run it off the main thread. Hidden names count only when `hidden` (the panes show them).
  [[nodiscard]] Result compare(const std::filesystem::path& left, const std::filesystem::path& right, bool hidden);

} // namespace folder_compare
