#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Batch rename rules (DESIGN.md "Batch Rename", FILES-UX F2 on 2+ items): one rule over the selected names, with the
// result per item checked before anything is renamed.
struct BatchRenameRule {
  enum class Mode : std::uint8_t { Replace, Add, Format, Case, Regex };
  enum class CaseMode : std::uint8_t { Lower, Upper, Title };

  Mode mode = Mode::Replace;
  std::string find;        // Replace: plain text; Regex: ECMAScript pattern
  std::string replacement; // Replace, Regex ($1 back references)
  bool matchCase = false;  // Replace
  std::string text;        // Add
  bool addBefore = false;  // Add: prefix instead of suffix
  // Format: {name} the old stem, {n} the counter, {date} the modified date, {taken} the date taken (else modified),
  // as YYYY-MM-DD.
  std::string format = "{name} {n}";
  int start = 1;
  int digits = 1; // the counter pads with zeros to this width
  CaseMode caseMode = CaseMode::Lower;
  bool keepExtension = true; // the rule sees only the stem; the extension stays as it is
};

struct BatchRenameItem {
  std::string name;
  std::chrono::system_clock::time_point modified{};
  std::optional<std::chrono::system_clock::time_point> taken;
};

struct BatchRenameResult {
  std::string name;  // the new name; equal to the old one when the rule changes nothing
  std::string error; // why this name cannot be used ("" = fine)
  bool conflict = false; // another result or an existing file that stays has this name
};

// `existing` = every name in the folder. Names of the renamed items count as free, since they move too.
[[nodiscard]] std::vector<BatchRenameResult> applyBatchRename(const BatchRenameRule& rule,
                                                              const std::vector<BatchRenameItem>& items,
                                                              const std::set<std::string>& existing);
