// Crawl a temp tree and check what name search finds: skip rules, short queries, ranking, re-crawl after delete.
#include "crawler.h"
#include "index_db.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

  void touch(const fs::path& p) {
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
} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("index-test-" + std::to_string(getpid()));
  touch(root / "Documents/Quarterly Report.pdf");
  touch(root / "Documents/report-draft.txt");
  touch(root / "Documents/notes/Report");
  touch(root / "code/repo/.git/HEAD");
  touch(root / "code/repo/secret-report.c");
  touch(root / ".hidden/report.txt");
  touch(root / "app/node_modules/report.js");
  touch(root / "ab.txt");
  touch(root / ".git/HEAD");

  {
    IndexDb db(root / "state/index.db", true);
    check(db.ok(), "database opens");
    crawl(db, root);

    check(found(db, "report", "Quarterly Report.pdf"), "substring match, case-insensitive");
    check(found(db, "quart", "Quarterly Report.pdf"), "name prefix");
    check(!found(db, "report", "secret-report.c"), "folder with .git is skipped");
    check(!found(db, "report", "report.txt"), "hidden folder is skipped");
    check(!found(db, "report", "report.js"), "node_modules is skipped");
    check(db.queryNames("ab", 50).empty(), "a query under 3 characters returns nothing");
    check(found(db, "quart", "Quarterly Report.pdf"), "a root that is a git checkout is still indexed");

    const auto rows = db.queryNames("report", 10);
    check(!rows.empty() && rows.front().name == "Report", "exact name ranks first");

    // A deleted newest entry frees its id for the next add(); the old name must not come back with it.
    const auto docs = db.idOf((root / "Documents").string());
    check(docs.has_value(), "folder id is found");
    const std::int64_t swap = db.add(*docs, "zz-swap-file.tmp", 1, 0, false);
    db.removeTree(swap);
    const std::int64_t fresh = db.add(*docs, "zz-fresh-notes.txt", 2, 0, false);
    db.update(fresh, 3, 0);
    check(db.queryNames("zz-swap", 10).empty(), "a removed name does not come back on id reuse");
    check(found(db, "zz-fresh", "zz-fresh-notes.txt"), "an entry added after a removal is found");
    fs::remove(root / "Documents/report-draft.txt");
    db.removeTree((root / "Documents/report-draft.txt").string());
    check(!found(db, "draft", "report-draft.txt"), "removed file is gone");
    db.removeTree((root / "Documents").string());
    check(!found(db, "quart", "Quarterly Report.pdf"), "removed tree is gone");
  }

  fs::remove_all(root);
  std::printf(g_failures == 0 ? "PASS\n" : "%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
