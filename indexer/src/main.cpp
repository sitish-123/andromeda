#include "crawler.h"
#include "index_db.h"
#include "live_index.h"

#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
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
    std::fprintf(stderr, "usage: andromeda-index crawl [root] | watch [root] | query <text> [limit]\n");
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
  if (command == "watch") {
    constexpr int kQuietMs = 200;
    constexpr int kSweepMs = 10 * 60 * 1000;
    const std::filesystem::path root = argc > 2 ? std::filesystem::path(argv[2]) : envPath("HOME", "");
    IndexDb db(dbPath());
    if (!db.ok()) {
      return 1;
    }
    LiveIndex live(db, root);
    const int fd = live.start();
    if (fd < 0) {
      return 1;
    }
    // Changes made while no watcher ran are found by one sweep at start.
    const auto sweepStart = std::chrono::steady_clock::now();
    live.sweep();
    std::fprintf(stderr, "watching %zu folders; start sweep %lld ms\n", live.watchCount(),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - sweepStart)
                                            .count()));
    auto nextSweep = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSweepMs);
    while (true) {
      // Events only mark folders dirty; rescans wait for kQuietMs without events, so a burst costs one pass.
      const auto untilSweep = std::chrono::duration_cast<std::chrono::milliseconds>(nextSweep - std::chrono::steady_clock::now());
      pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
      const int ready = poll(&p, 1, live.dirty() ? kQuietMs : static_cast<int>(std::max<long long>(0, untilSweep.count())));
      if (ready > 0) {
        live.readEvents();
      } else if (live.dirty()) {
        live.flush();
      } else {
        live.sweep();
        nextSweep = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSweepMs);
      }
    }
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
