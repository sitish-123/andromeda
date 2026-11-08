#pragma once

#include "ui/files/batch_rename.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Button;
class Flex;
class Label;
class Node;

// Batch Rename sheet (DESIGN.md "Batch Rename", rulebook 9.10 dialog): F2 on 2+ items. One rule, a live old -> new
// list with conflicts marked, and Rename runs one job (one undo step).
class BatchRenameSheet {
public:
  using Rename = std::function<void(std::vector<std::pair<std::filesystem::path, std::string>>)>;

  BatchRenameSheet(std::filesystem::path folder, std::vector<BatchRenameItem> items, std::set<std::string> existing,
                   Rename rename, std::function<void()> close, std::function<void()> rebuild);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void submit();
  // E2E: mode (replace, add, format, case, pattern), find, replace, text, format, start, digits; the sheet rebuilds.
  void set(const std::string& field, const std::string& value);

private:
  void updatePreview();
  [[nodiscard]] std::unique_ptr<Node> buildFields(float scale);

  std::filesystem::path m_folder;
  std::vector<BatchRenameItem> m_items;
  std::set<std::string> m_existing;
  Rename m_rename;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  BatchRenameRule m_rule;
  std::vector<BatchRenameResult> m_results;

  Flex* m_list = nullptr;
  Label* m_summary = nullptr;
  Button* m_ok = nullptr;
  float m_scale = 1.0F;
};
