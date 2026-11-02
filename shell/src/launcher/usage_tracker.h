#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Tracks how many times each launcher result has been activated.
// Providers that opt in via LauncherProvider::trackUsage() get their results
// score-boosted based on activation history, surfacing frequently used entries.
class UsageTracker {
public:
  UsageTracker();

  void record(std::string_view providerId, std::string_view resultId);
  void clear();
  [[nodiscard]] int getCount(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] int getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] std::size_t getRecentlyUsedCount(std::string_view providerId);
  // Unix seconds of the last activation; nullopt for entries recorded before use times were kept.
  [[nodiscard]] std::optional<std::int64_t> getLastUsed(std::string_view providerId, std::string_view resultId);

private:
  void ensureLoaded();
  void save() const;

  std::string m_usageCountsPath;
  std::string m_recentlyUsedPath;
  std::string m_lastUsedPath;
  bool m_loaded = false;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_counts;
  std::unordered_map<std::string, std::deque<std::string>> m_recentlyUsed;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_recentlyUsedIndex;
  std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>> m_lastUsed;
};
