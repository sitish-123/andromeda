#include "trash.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <print>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "trash_test: {}", what);
      ++g_failures;
    }
  }

  void put(const fs::path& trash, const std::string& name, const std::string& info) {
    fs::create_directories(trash / "files");
    fs::create_directories(trash / "info");
    std::ofstream(trash / "files" / name) << "x";
    std::ofstream(trash / "info" / (name + ".trashinfo")) << info;
  }
} // namespace

int main() {
  char tmpl[] = "/tmp/files-trash-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  ::setenv("XDG_DATA_HOME", root.c_str(), 1);
  const fs::path trash = homeTrashDir();
  expect(trash == root / "Trash", "the home trash is $XDG_DATA_HOME/Trash");
  expect(!trashDirs().empty() && trashDirs().front() == trash, "the home trash comes first");

  put(trash, "a b.txt", "[Trash Info]\nPath=/home/u/Docs/a%20b.txt\nDeletionDate=2026-11-05T07:00:00\n");
  put(trash, "orphan-info-missing", "");
  fs::remove(trash / "info" / "orphan-info-missing.trashinfo");
  std::ofstream(trash / "info" / "gone.trashinfo") << "[Trash Info]\nPath=/x/gone\n";
  put(trash, "other.txt", "[Other]\nPath=/wrong\n[Trash Info]\nPath=/home/u/other.txt\n");

  auto items = listTrash(trash);
  std::ranges::sort(items, {}, &TrashItem::name);
  expect(items.size() == 2, "only items with both info and files are listed: " + std::to_string(items.size()));
  if (items.size() == 2) {
    expect(items[0].name == "a b.txt" && items[0].originalPath == "/home/u/Docs/a b.txt", "Path= is percent-decoded");
    expect(items[0].deletionDate == "2026-11-05T07:00:00", "DeletionDate= is read");
    expect(items[1].originalPath == "/home/u/other.txt", "only the [Trash Info] group counts");
  }
  expect(decodeTrashPath("dir/f.txt", "/mnt/usb/.Trash-1000") == "/mnt/usb/dir/f.txt",
         "a relative Path= is relative to the volume top (.Trash-uid)");
  expect(decodeTrashPath("f.txt", "/mnt/usb/.Trash/1000") == "/mnt/usb/f.txt",
         "a relative Path= is relative to the volume top (.Trash/uid)");

  fs::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
