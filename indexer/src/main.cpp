#include "crawler.h"
#include "index_db.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

  std::filesystem::path envPath(const char* name, const char* homeFallback) {
    if (const char* v = std::getenv(name); v != nullptr && v[0] != '\0') {
      return v;
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/tmp") / homeFallback;
  }

  std::filesystem::path dbPath() { return envPath("XDG_STATE_HOME", ".local/state") / "andromeda" / "index.db"; }

  int usage() {
    std::fprintf(stderr, "usage: andromeda-index crawl [root] | query <text> [limit]\n");
    return 2;
  }

} // namespace

int main(int argc, char** argv) {
  using namespace andromeda::index;
  if (argc < 2) {
    return usage();
  }
  IndexDb db(dbPath());
  if (!db.ok()) {
    return 1;
  }
  const std::string command = argv[1];
  if (command == "crawl") {
    const std::filesystem::path home = envPath("HOME", "");
    const std::filesystem::path root = argc > 2 ? std::filesystem::path(argv[2]) : home;
    std::vector<std::filesystem::path> first;
    for (const char* folder : {"Desktop", "Documents", "Downloads"}) {
      if (std::filesystem::is_directory(root / folder)) {
        first.push_back(root / folder);
      }
    }
    const auto start = std::chrono::steady_clock::now();
    const CrawlStats stats = crawl(db, root, first);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("%lld files, %lld folders, %lld skipped folders in %lld ms\n", static_cast<long long>(stats.files),
                static_cast<long long>(stats.folders), static_cast<long long>(stats.skippedFolders),
                static_cast<long long>(ms.count()));
    return 0;
  }
  if (command == "query" && argc > 2) {
    const int limit = argc > 3 ? std::atoi(argv[3]) : 20;
    const auto start = std::chrono::steady_clock::now();
    const auto rows = db.queryNames(argv[2], limit);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    for (const FileRow& row : rows) {
      std::printf("%s\n", row.path.c_str());
    }
    std::fprintf(stderr, "%zu rows in %lld us\n", rows.size(), static_cast<long long>(us.count()));
    return 0;
  }
  return usage();
}
