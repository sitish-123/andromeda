#pragma once

#include "files/file_ops_client.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>
#include <memory>
#include <string>

class Node;
class ThumbnailService;

// The copy/move conflict sheet (FILES-UI-SPEC §14, mockup §5): both files side by side, a plain-language
// difference, apply-to-all, Stop · Skip · Keep Both · Replace. A click on a card picks the file to keep.
class ConflictSheet {
public:
  enum class Choice : std::uint8_t { Replace, Skip, KeepBoth, Stop };

  struct Info {
    std::string verb; // "Copying", "Moving"
    std::filesystem::path source;
    std::filesystem::path target; // the existing file
    std::uint64_t totalFiles = 0;
    std::uint64_t conflictIndex = 0; // 1-based, 0 = unknown
    std::uint64_t conflictTotal = 0; // 0 = unknown
    std::optional<FileOpsClient::Job::FolderClash> folderClash; // a folder onto a folder: the Merge sheet
  };

  ConflictSheet(Info info, ThumbnailService* thumbnails, std::function<void(Choice, bool applyToAll)> answer,
                std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void choose(Choice choice);

  // The name Keep Both saves, by the daemon's rule (ops.rs free_name): "name (copy).ext", "name (copy 2).ext", …
  [[nodiscard]] static std::string keepBothName(const std::filesystem::path& target);
  // "The copy is 200 MB larger and 5 weeks newer." from both sizes and times.
  [[nodiscard]] static std::string difference(std::uint64_t newSize, std::int64_t newTimeNs, std::uint64_t oldSize,
                                              std::int64_t oldTimeNs);

private:
  [[nodiscard]] std::unique_ptr<Node> buildMerge(float scale);

  Info m_info;
  ThumbnailService* m_thumbnails;
  std::function<void(Choice, bool)> m_answer;
  std::function<void()> m_rebuild;
  bool m_keepNew = true; // the selected card: the copy (Replace) or the one already there (Skip)
  bool m_applyToAll = false;
  bool m_answered = false;
};
