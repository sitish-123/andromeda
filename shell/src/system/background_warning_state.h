#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

// What the background warning already told the user, kept across boots in one JSON file:
// a finding notifies once per unit file path + mtime (a changed file may notify again), and ignored rows never do.
class BackgroundWarningState {
public:
  explicit BackgroundWarningState(std::string path);

  // key: the unit's FragmentPath, or the unit name when it has no file. mtime: the file's st_mtime in seconds.
  [[nodiscard]] bool alreadyNotified(const std::string& key, std::int64_t mtime) const;
  void markNotified(const std::string& key, std::int64_t mtime);

  // Ignored rows by ActivityRow::key; listed under "Ignored", never notify.
  [[nodiscard]] bool ignored(const std::string& rowKey) const { return m_ignored.contains(rowKey); }
  [[nodiscard]] const std::set<std::string>& ignoredKeys() const noexcept { return m_ignored; }
  void setIgnored(const std::string& rowKey, bool ignored);

private:
  void save() const;

  std::string m_path;
  std::map<std::string, std::int64_t> m_notified;
  std::set<std::string> m_ignored;
};

// st_mtime of path in seconds, or 0 when it does not exist.
[[nodiscard]] std::int64_t fileMtime(const std::string& path);
