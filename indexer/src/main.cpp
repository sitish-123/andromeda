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
  const std::string command = argv[1];
  if (command == "crawl") {
    const std::filesystem::path root = argc > 2 ? std::filesystem::path(argv[2]) : envPath("HOME", "");
    // A crawl builds a fresh file and swaps it in, so readers never see a half-built index.
    const std::filesystem::path target = dbPath();
    std::filesystem::path fresh = target;
    fresh += ".new";
    std::filesystem::remove(fresh);
    const auto start = std::chrono::steady_clock::now();
    CrawlStats stats;
    {
      IndexDb bulk(fresh, true);
      if (!bulk.ok()) {
        return 1;
      }
      stats = crawl(bulk, root);
      bulk.finishBulk();
    }
    std::filesystem::rename(fresh, target);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("%lld files, %lld folders, %lld skipped folders in %lld ms (walk %lld, db writer %lld)\n",
                static_cast<long long>(stats.files), static_cast<long long>(stats.folders),
                static_cast<long long>(stats.skippedFolders), static_cast<long long>(ms.count()),
                static_cast<long long>(stats.walkMs), static_cast<long long>(stats.dbMs));
    return 0;
  }
  if (command == "query" && argc > 2) {
    const IndexDb db(dbPath());
    if (!db.ok()) {
      return 1;
    }
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
