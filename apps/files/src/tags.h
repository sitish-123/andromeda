#pragma once

#include "ui/files/file_browser.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Tags in the freedesktop xattr `user.xdg.tags` (comma list; KDE and GNOME read the same one). Writes go through
// andromeda-fileops SetTags (undo); this file only reads them, keeps the tag config and finds tagged files.
struct TagDef {
  std::string name;
  std::uint32_t rgb = 0; // 0xRRGGBB
  bool sidebar = true;
};

struct SidebarTag {
  std::string name;
  std::uint32_t rgb = 0;
  std::size_t count = 0;
};

[[nodiscard]] std::vector<std::string> parseTagList(std::string_view raw);
[[nodiscard]] std::vector<std::string> readTags(const std::filesystem::path& path);

// $XDG_CONFIG_HOME/andromeda/files/tags.json: [{"name","color":"#rrggbb","sidebar"}] in sidebar order.
[[nodiscard]] std::filesystem::path tagConfigPath();
// The Finder-style defaults when the file is missing or broken.
[[nodiscard]] std::vector<TagDef> loadTagConfig();
bool saveTagConfig(const std::vector<TagDef>& tags);

// Tagged files under home, found by a walk on a worker thread (the indexer replaces it later). The result stays
// cached for 10 min; `changed` runs on the main loop after each walk.
class TagIndex : public std::enable_shared_from_this<TagIndex> {
public:
  explicit TagIndex(std::function<void()> changed, std::filesystem::path root = {});
  ~TagIndex();

  // Walks again unless a walk runs or a recent result exists (fresh: always).
  void refresh(bool fresh = false);
  // Updates the cache after a SetTags job on these paths, without a walk.
  void reread(const std::vector<std::filesystem::path>& paths);
  [[nodiscard]] bool ready() const { return m_ready; }
  // Runs `then` on the main loop after a walk that started after this call (tag rename and delete need every file).
  void walkThen(std::function<void()> then);
  [[nodiscard]] std::vector<std::filesystem::path> paths(const std::string& tag) const;
  [[nodiscard]] std::size_t count(const std::string& tag) const;
  // The tags of one file as of the last walk (memory only; Group By Tags asks per row).
  [[nodiscard]] const std::vector<std::string>& tagsOf(const std::filesystem::path& path) const;
  void configChanged() {
    if (m_changed) {
      m_changed();
    }
  }
  // Tag jobs whose undo needs a fresh walk.
  void addJob(std::string job) { m_jobs.insert(std::move(job)); }
  [[nodiscard]] bool ownsJob(const std::string& job) const { return m_jobs.contains(job); }
  // Tags on files that the config does not name.
  [[nodiscard]] std::vector<std::string> unlisted(const std::vector<TagDef>& config) const;

  // Config order first (sidebar = true), then tags found on files that the config does not name (grey).
  [[nodiscard]] std::vector<SidebarTag> sidebar() const;
  // id "tag:<name>"; rows have absolute names, newest first.
  [[nodiscard]] FileBrowser::Listing listing(const std::string& tag) const;

private:
  void walk();

  std::function<void()> m_changed;
  std::filesystem::path m_root;
  std::map<std::string, std::vector<std::filesystem::path>> m_byTag;
  mutable std::map<std::string, std::vector<std::string>> m_byPath; // built from m_byTag on demand
  mutable bool m_byPathStale = true;
  std::shared_ptr<std::atomic<bool>> m_stop = std::make_shared<std::atomic<bool>>(false);
  std::int64_t m_walkedAt = 0;
  bool m_walking = false;
  bool m_walkAgain = false;
  std::vector<std::filesystem::path> m_rereadDuringWalk;
  bool m_ready = false;
  std::set<std::string> m_jobs;
  std::uint64_t m_walks = 0; // walks started
  std::vector<std::pair<std::uint64_t, std::function<void()>>> m_afterWalk; // runs once that walk has landed
};
