// A crawled temp tree changes on disk; the live index must show each change: by inotify within 1 s in watched
// folders, by sweep() in a folder too old to be watched.
#include "crawler.h"
#include "index_db.h"
#include "live_index.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace andromeda::index;

namespace {
  int g_failures = 0;

  void check(bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++g_failures;
    }
  }

  void write(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
  }

  bool found(const IndexDb& db, const char* query, const std::string& name) {
    for (const FileRow& row : db.queryNames(query, 50)) {
      if (row.name == name) {
        return true;
      }
    }
    return false;
  }

  // Reads events until 100 ms pass without one (at most 1 s), then applies them.
  void settle(LiveIndex& live, int fd) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    pollfd p{.fd = fd, .events = POLLIN, .revents = 0};
    while (std::chrono::steady_clock::now() < deadline && poll(&p, 1, 100) > 0) {
      live.readEvents();
    }
    live.flush();
  }

  void makeOld(const fs::path& p) {
    const timespec old[2] = {{.tv_sec = 1'000'000'000, .tv_nsec = 0}, {.tv_sec = 1'000'000'000, .tv_nsec = 0}};
    utimensat(AT_FDCWD, p.c_str(), old, 0);
  }
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("live-test-" + std::to_string(getpid()));
  write(root / "Documents/start.txt");
  write(root / "Archive/2001/old-notes.txt");
  makeOld(root / "Archive/2001");
  makeOld(root / "Archive");
  {
    IndexDb bulk(root / "state/index.db", true);
    crawl(bulk, root);
    bulk.finishBulk();
  }
  IndexDb db(root / "state/index.db");
  LiveIndex live(db, root);
  const int fd = live.start();
  check(fd >= 0, "inotify starts");

  write(root / "Documents/new-report.txt");
  settle(live, fd);
  check(found(db, "new-report", "new-report.txt"), "a new file is found");

  fs::rename(root / "Documents/new-report.txt", root / "Documents/renamed-report.txt");
  settle(live, fd);
  check(!found(db, "new-report", "new-report.txt"), "a renamed file loses its old name");
  check(found(db, "renamed", "renamed-report.txt"), "a renamed file is found by its new name");

  write(root / "Documents/project/sub/deep-note.md");
  settle(live, fd);
  check(found(db, "deep-note", "deep-note.md"), "a file in a new folder tree is found");
  write(root / "Documents/project/sub/later-note.md");
  settle(live, fd);
  check(found(db, "later-note", "later-note.md"), "a new folder is watched");

  fs::remove_all(root / "Documents/project");
  settle(live, fd);
  check(!found(db, "deep-note", "deep-note.md"), "a deleted tree is gone");

  write(root / "Archive/2001/late-file.txt");
  settle(live, fd);
  check(!found(db, "late-file", "late-file.txt"), "an old folder is not watched");
  live.sweep();
  check(found(db, "late-file", "late-file.txt"), "sweep finds a change in an unwatched folder");

  fs::create_directories(root / "Documents/repo");
  write(root / "Documents/repo/code-file.c");
  settle(live, fd);
  check(found(db, "code-file", "code-file.c"), "file in a plain folder is found");
  write(root / "Documents/repo/.git/HEAD");
  settle(live, fd);
  check(!found(db, "code-file", "code-file.c"), "a folder that becomes a git checkout is emptied");

  fs::remove_all(root);
  std::printf(g_failures == 0 ? "PASS\n" : "%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
