#include "archive_listing.h"

#include <print>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "archive_listing_test: {}", what);
      ++g_failures;
    }
  }

  std::string rows(const FileBrowser::Listing& listing) {
    std::string out;
    for (const auto& row : listing.rows) {
      out += (out.empty() ? "" : ",") + row->displayName + (row->isDir ? "/" : "");
    }
    return out;
  }
} // namespace

int main() {
  // Most zips have no record for a folder that only holds files: "docs" and "docs/sub" exist only as path parts.
  const std::vector<FileOpsClient::ArchiveEntry> entries{
      {.path = "docs/a.txt", .kind = "file", .size = 5, .mtime = 1, .isDir = false},
      {.path = "docs/sub/b.txt", .kind = "file", .size = 7, .mtime = 1, .isDir = false},
      {.path = "empty", .kind = "dir", .size = 0, .mtime = 1, .isDir = true},
      {.path = "top.txt", .kind = "file", .size = 1, .mtime = 1, .isDir = false},
  };
  const ArchiveListing archive("/home/u/photos.zip", entries);

  const auto root = archive.listing();
  expect(rows(root) == "docs/,empty/,top.txt", "the top shows implied and recorded folders and files: " + rows(root));
  expect(root.id == "archive:/home/u/photos.zip", "the root id names the archive: " + root.id);
  expect(rows(archive.listing("docs")) == "a.txt,sub/", "a nested folder lists its own children: " + rows(archive.listing("docs")));
  expect(rows(archive.listing("docs/sub")) == "b.txt", "a folder two levels down lists its file");
  expect(!root.rows.empty() && root.rows.front()->size == 12, "a folder shows the bytes under it");
  expect(archive.innerOf("/home/u/photos.zip/docs/sub") == std::filesystem::path("docs/sub"), "a row name maps back to its inner path");
  expect(!archive.innerOf("/home/u/other/x").has_value(), "a path outside the archive has no inner path");
  expect(archive.isFolder("docs/sub") && !archive.isFolder("top.txt"), "folders are known, files are not folders");
  expect(archive.topNames() == std::vector<std::string>{"docs", "empty", "top.txt"}, "the top names feed Extract Here");
  expect(ArchiveListing::isArchive("/x/a.tar.zst") && !ArchiveListing::isArchive("/x/a.txt"), "archives are known by name");
  return g_failures == 0 ? 0 : 1;
}
