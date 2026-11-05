#include "core/freedesktop_thumbnails.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <print>
#include <string>
#include <sys/stat.h>
#include <utime.h>

namespace fs = std::filesystem;

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "freedesktop_thumbnails_test: {}", what);
      ++g_failures;
    }
  }

  void chunk(std::ofstream& out, const std::string& type, const std::string& data) {
    const auto n = static_cast<std::uint32_t>(data.size());
    const char len[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    out.write(len, 4);
    out << type << data;
    out.write("\0\0\0\0", 4); // the lookup reads text chunks only; it does not check CRCs
  }

  // A PNG skeleton with the thumbnail text chunks of the standard.
  void writeThumb(const fs::path& png, const std::string& uri, long long mtime) {
    fs::create_directories(png.parent_path());
    std::ofstream out(png, std::ios::binary);
    out.write("\x89PNG\r\n\x1a\n", 8);
    chunk(out, "IHDR", std::string(13, '\0'));
    chunk(out, "tEXt", std::string("Thumb::URI") + '\0' + uri);
    chunk(out, "tEXt", std::string("Thumb::MTime") + '\0' + std::to_string(mtime));
    chunk(out, "IEND", "");
  }

  std::string md5(const std::string& s) {
    char* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, s.c_str(), -1);
    std::string out(sum);
    g_free(sum);
    return out;
  }
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("fdo-thumbs-" + std::to_string(getpid()));
  fs::create_directories(root);
  // Before the first g_get_user_cache_dir(): glib reads it once.
  setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
  const fs::path file = root / "a doc.pdf";
  std::ofstream(file) << "%PDF";
  utimbuf times{1700000000, 1700000000};
  utime(file.c_str(), &times);
  char* uriC = g_filename_to_uri(file.c_str(), nullptr, nullptr);
  const std::string uri = uriC;
  g_free(uriC);
  const fs::path cache = root / "cache" / "thumbnails";

  using freedesktop_thumbnails::knownFailure;
  using freedesktop_thumbnails::lookup;

  expect(!lookup(file, 192), "no thumbnail before one is written");

  // A thumbnail for this mtime is found; a larger size serves a smaller request.
  writeThumb(cache / "x-large" / (md5(uri) + ".png"), uri, 1700000000);
  const auto hit = lookup(file, 192);
  expect(hit && hit->parent_path().filename() == "x-large", "x-large thumbnail serves a 192 px request");

  // An edited file makes the thumbnail stale.
  utimbuf later{1700000100, 1700000100};
  utime(file.c_str(), &later);
  expect(!lookup(file, 192), "a thumbnail of the old mtime is not used");

  // A thumbnail written for another file under the same name (wrong URI) is not used.
  writeThumb(cache / "large" / (md5(uri) + ".png"), "file:///elsewhere.pdf", 1700000100);
  expect(!lookup(file, 192), "a thumbnail of another URI is not used");

  // A failure marker for this mtime stops a retry; after an edit it does not.
  writeThumb(cache / "fail" / "some-thumbnailer" / (md5(uri) + ".png"), uri, 1700000100);
  expect(knownFailure(file), "a fail marker for this mtime is a known failure");
  utime(file.c_str(), &times);
  expect(!knownFailure(file), "a fail marker of another mtime is not");

  std::error_code ec;
  fs::remove_all(root, ec);
  return g_failures == 0 ? 0 : 1;
}
