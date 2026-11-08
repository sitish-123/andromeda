#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class FileOpsClient;
class Node;

// Storage page (mockup files-v2-live.html §12) for FileBrowser::Listing::page: used of capacity, the category bar,
// Largest folders with drill-down, and Recommendations (Empty Trash, old downloads, app caches) with an inline
// confirm. The scan runs in andromeda-fileops (StorageScan); the host keeps this object alive while the page shows.
class StoragePage : public std::enable_shared_from_this<StoragePage> {
public:
  struct Actions {
    std::function<void()> rebuild;
    std::function<void()> emptyTrash;                                   // for good; the page has confirmed
    std::function<void(std::vector<std::filesystem::path>)> trash;      // with the host's undo toast
    std::function<void(std::vector<std::filesystem::path>)> remove;     // for good; the page has confirmed
    std::function<void(const std::filesystem::path&)> open;             // show the folder in Files
  };

  // The JSON Result of a StorageScan job.
  struct Summary {
    std::uint64_t total = 0;
    std::vector<std::pair<std::string, std::uint64_t>> categories;
    std::vector<std::pair<std::filesystem::path, std::uint64_t>> largest;
    std::uint64_t trash = 0;
    std::uint64_t caches = 0;
    std::uint64_t oldDownloads = 0;
    std::vector<std::filesystem::path> oldDownloadItems;
  };
  [[nodiscard]] static std::optional<Summary> parse(const std::string& json);

  StoragePage(FileOpsClient* ops, std::filesystem::path root, Actions actions);

  // Starts a scan (a recent one is reused unless fresh).
  void scan(bool fresh = false);
  [[nodiscard]] std::unique_ptr<Node> build(float scale);

private:
  enum class Confirm : std::uint8_t { None, EmptyTrash, OldDownloads, Caches };
  struct Row {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    bool isDir = false;
  };

  void drill(const std::filesystem::path& folder);
  void later(std::function<void()> fn);
  [[nodiscard]] std::unique_ptr<Node> buildBar(float s) const;
  [[nodiscard]] std::unique_ptr<Node> buildFolders(float s);
  [[nodiscard]] std::unique_ptr<Node> buildRecommendations(float s);

  FileOpsClient* m_ops;
  std::filesystem::path m_root;
  Actions m_actions;
  bool m_scanning = false;
  std::string m_error;
  std::optional<Summary> m_summary;
  std::optional<std::filesystem::path> m_drill;
  std::vector<Row> m_drillRows;
  Confirm m_confirm = Confirm::None;
};
