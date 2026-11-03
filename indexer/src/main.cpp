#include "content_worker.h"
#include "crawler.h"
#include "index_db.h"
#include "live_index.h"
#include "query_server.h"

#include <malloc.h>
#include <poll.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

  std::filesystem::path envPath(const char* name, const char* homeFallback) {
    if (const char* v = std::getenv(name); v != nullptr && v[0] != '\0') {
      return v;
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/tmp") / homeFallback;
  }

  std::filesystem::path dbPath() { return envPath("XDG_STATE_HOME", ".local/state") / "andromeda" / "index.db"; }

  std::filesystem::path socketPath() { return envPath("XDG_RUNTIME_DIR", ".cache") / "andromeda-index.sock"; }

  int usage() {
    std::fprintf(stderr, "usage: andromeda-index crawl [root] | serve [root] | watch [root] | query <text> [limit]\n");
    return 2;
  }

  // A crawl builds a fresh file and swaps it in, so readers never see a half-built index.
  bool crawlInto(const std::filesystem::path& target, const std::filesystem::path& root) {
    std::filesystem::path fresh = target;
    fresh += ".new";
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::remove(fresh);
    const auto start = std::chrono::steady_clock::now();
    andromeda::index::CrawlStats stats;
    {
      andromeda::index::IndexDb bulk(fresh, true);
      if (!bulk.ok()) {
        return false;
      }
      stats = andromeda::index::crawl(bulk, root);
      bulk.finishBulk();
    }
    std::filesystem::rename(fresh, target);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("%lld files, %lld folders, %lld skipped folders in %lld ms (walk %lld, db writer %lld)\n",
                static_cast<long long>(stats.files), static_cast<long long>(stats.folders),
                static_cast<long long>(stats.skippedFolders), static_cast<long long>(ms.count()),
                static_cast<long long>(stats.walkMs), static_cast<long long>(stats.dbMs));
    std::fflush(stdout);
    return true;
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
    return crawlInto(dbPath(), root) ? 0 : 1;
  }
  if (command == "watch" || command == "serve") {
    constexpr int kQuietMs = 200;
    constexpr int kSweepMs = 10 * 60 * 1000;
    const bool serve = command == "serve";
    // A write to an extract helper that died must fail with EPIPE, not end the daemon.
    std::signal(SIGPIPE, SIG_IGN);
    // One heap for both threads, so malloc_trim gives back what the content worker freed.
    mallopt(M_ARENA_MAX, 1);
    const std::filesystem::path root = argc > 2 ? std::filesystem::path(argv[2]) : envPath("HOME", "");
    // The socket listens before the first crawl, so early clients wait in the backlog instead of failing.
    const int listenFd = serve ? QueryServer::listenSocket(socketPath()) : -1;
    if (serve && listenFd < 0) {
      std::perror("andromeda-index: socket");
      return 1;
    }
    if (!std::filesystem::exists(dbPath()) && !crawlInto(dbPath(), root)) {
      return 1;
    }
    IndexDb db(dbPath());
    if (!db.ok()) {
      return 1;
    }
    std::optional<QueryServer> server;
    std::optional<ContentWorker> content;
    if (serve) {
      db.loadNames();
      server.emplace(db, listenFd);
      content.emplace(dbPath());
    }
    LiveIndex live(db, root);
    const int fd = live.start();
    if (fd < 0) {
      return 1;
    }
    // Changes made while no watcher ran are found by one sweep at start.
    const auto sweepStart = std::chrono::steady_clock::now();
    live.sweep();
    // A sweep's folder list is freed at once; give the pages back instead of keeping them in the heap.
    malloc_trim(0);
    std::fprintf(stderr, "watching %zu folders; start sweep %lld ms\n", live.watchCount(),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - sweepStart)
                                            .count()));
    using Clock = std::chrono::steady_clock;
    auto nextSweep = Clock::now() + std::chrono::milliseconds(kSweepMs);
    auto quietAt = Clock::now();
    while (true) {
      // Events only mark folders dirty; rescans wait for kQuietMs after the last event, so a burst costs one
      // pass. Queries do not move that deadline.
      const auto wakeAt = live.dirty() ? quietAt : nextSweep;
      const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(wakeAt - Clock::now()).count();
      std::vector<pollfd> fds{{.fd = fd, .events = POLLIN, .revents = 0}};
      if (server) {
        server->addPollFds(fds);
      }
      if (poll(fds.data(), fds.size(), static_cast<int>(std::max<long long>(0, timeout))) > 0) {
        if (fds[0].revents != 0) {
          live.readEvents();
          quietAt = Clock::now() + std::chrono::milliseconds(kQuietMs);
        }
        if (server) {
          server->handle(std::span(fds).subspan(1));
        }
      }
      const auto now = Clock::now();
      if (live.dirty() && now >= quietAt) {
        live.flush();
        if (content) {
          content->wakeLive();
        }
      } else if (!live.dirty() && now >= nextSweep) {
        live.sweep();
        if (content) {
          content->wake();
        }
        malloc_trim(0);
        nextSweep = Clock::now() + std::chrono::milliseconds(kSweepMs);
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
