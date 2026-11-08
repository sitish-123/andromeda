#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct FileInfo;
class IndexSearch;

// Smart folders (mockup files-v2-live.html §16, Finder saved searches): a search kept as a sidebar place that runs
// again on each visit. Stored in $XDG_CONFIG_HOME/andromeda/files/smart-folders.json in sidebar order.
struct SmartFolder {
  std::string name;
  std::string text;             // name contains (any case); "" = any name
  std::string kind;             // "" any, "pdf", "image", "doc", "sheet" (the index kind words)
  int days = 0;                 // modified in the last N days; 0 = any time
  std::filesystem::path folder; // empty = This PC

  // "Kind is Image · Modified in the last 30 days · in This PC"
  [[nodiscard]] std::string sentence() const;
  bool operator==(const SmartFolder&) const = default;
};

namespace smart_folders {
  [[nodiscard]] std::filesystem::path configPath();
  [[nodiscard]] std::vector<SmartFolder> load();
  bool save(const std::vector<SmartFolder>& folders);

  using Rows = std::vector<std::shared_ptr<const FileInfo>>;
  // Runs the search: This PC through the index when its daemon runs, else (and for a folder) a walk on a worker thread
  // (hidden folders skipped, at most kWalkLimit entries). `done` runs on the main loop; rows have absolute names,
  // newest first. `alive` expired = the caller is gone and `done` does not run.
  void run(const SmartFolder& folder, IndexSearch* index, std::weak_ptr<bool> alive, std::function<void(Rows)> done);
  inline constexpr std::size_t kWalkLimit = 200'000;
  inline constexpr std::size_t kMaxRows = 2'000;
} // namespace smart_folders
