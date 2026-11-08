#include "ui/files/batch_rename.h"

#include <print>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "batch_rename_test: {}", what);
      ++g_failures;
    }
  }

  std::vector<BatchRenameItem> items(std::initializer_list<const char*> names) {
    std::vector<BatchRenameItem> out;
    for (const char* n : names) {
      out.push_back({.name = n});
    }
    return out;
  }
} // namespace

int main() {
  using Mode = BatchRenameRule::Mode;
  const auto files = items({"IMG_001.JPG", "IMG_002.JPG", "notes.txt"});

  auto r = applyBatchRename({.mode = Mode::Replace, .find = "img_", .replacement = "Trip "}, files, {});
  expect(r[0].name == "Trip 001.JPG" && r[2].name == "notes.txt", "replace ignores case and keeps the extension");

  r = applyBatchRename({.mode = Mode::Replace, .find = "img_", .replacement = "x", .matchCase = true}, files, {});
  expect(r[0].name == "IMG_001.JPG", "match case finds nothing");

  BatchRenameRule format{.mode = Mode::Format, .format = "Trip {n}", .start = 9, .digits = 3};
  r = applyBatchRename(format, files, {});
  expect(r[0].name == "Trip 009.JPG" && r[2].name == "Trip 011.txt", "format counts from start with padding");

  BatchRenameRule taken{.mode = Mode::Format, .format = "{taken} {name}"};
  auto dated = items({"a.jpg", "b.jpg"});
  dated[0].taken = std::chrono::sys_days{std::chrono::year{2026} / 7 / 4};
  dated[1].modified = std::chrono::sys_days{std::chrono::year{2025} / 1 / 2};
  r = applyBatchRename(taken, dated, {});
  expect(r[0].name == "2026-07-04 a.jpg" && r[1].name == "2025-01-02 b.jpg", "taken falls back to modified");

  r = applyBatchRename({.mode = Mode::Format, .format = "same"}, files, {});
  expect(r[0].conflict && r[1].conflict && !r[2].conflict, "two results with one name conflict");

  r = applyBatchRename({.mode = Mode::Add, .text = "-old"}, items({"a.txt"}), {"a.txt", "a-old.txt"});
  expect(r[0].conflict, "a name an untouched file has conflicts");

  r = applyBatchRename({.mode = Mode::Replace, .find = "a", .replacement = "b"}, items({"a.txt", "b.txt"}),
                       {"a.txt", "b.txt"});
  expect(r[0].conflict, "a.txt -> b.txt collides with b.txt, which stays b.txt");

  r = applyBatchRename({.mode = Mode::Replace, .find = "1", .replacement = "2"}, items({"f1.txt", "f2.txt"}),
                       {"f1.txt", "f2.txt"});
  expect(r[0].conflict && r[1].conflict, "f1 -> f2 and f2 (unchanged) end on one name");

  r = applyBatchRename({.mode = Mode::Case, .caseMode = BatchRenameRule::CaseMode::Title}, items({"élan vital.md"}), {});
  expect(r[0].name == "Élan Vital.md", "title case handles accents");

  r = applyBatchRename({.mode = Mode::Regex, .find = "IMG_(\\d+)", .replacement = "photo-$1"}, files, {});
  expect(r[1].name == "photo-002.JPG", "regex back references");

  r = applyBatchRename({.mode = Mode::Regex, .find = "(", .replacement = ""}, files, {});
  expect(!r[0].error.empty(), "a bad pattern is an error, not a crash");

  r = applyBatchRename({.mode = Mode::Replace, .find = "notes", .replacement = "a/b"}, files, {});
  expect(!r[2].error.empty(), "slash is refused");

  if (g_failures == 0) {
    std::println("batch_rename_test: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
