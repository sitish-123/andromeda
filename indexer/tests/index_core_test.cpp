// Crawl a temp tree and check what name search finds: skip rules, short queries, ranking, re-crawl after delete.
#include "content_worker.h"
#include "crawler.h"
#include "index_db.h"
#include "query_server.h"

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
  touch(root / "Documents/Résumé Élan.pdf");
  touch(root / "Documents/ОТЧЁТ за год.txt");
  touch(root / "Documents/Straße Æon.md");
  touch(root / "Sdk/build-tools/NOTICE-tools.txt");
  touch(root / "code/app/builddir/build.ninja");
  touch(root / "code/app/builddir/generated-notice.txt");
  touch(root / "code/crate/target/CACHEDIR.TAG");
  touch(root / "code/crate/target/cargo-notice.txt");
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
    check(found(db, "notice-tools", "NOTICE-tools.txt"), "a folder named build-something is indexed");
    check(found(db, "resume elan", "Résumé Élan.pdf"), "accents are ignored in names");
    check(found(db, "RÉSUMÉ", "Résumé Élan.pdf"), "accented capitals match");
    check(found(db, "отчёт", "ОТЧЁТ за год.txt"), "Cyrillic names match without case");
    check(found(db, "strasse aeon", "Straße Æon.md"), "ß and Æ match ss and ae");
    check(!found(db, "generated-notice", "generated-notice.txt"), "a Ninja build folder is skipped");
    check(!found(db, "cargo-notice", "cargo-notice.txt"), "a folder with CACHEDIR.TAG is skipped");
    check(db.queryNames("ab", 50).empty(), "a query under 3 characters returns nothing");
    check(found(db, "quart", "Quarterly Report.pdf"), "a root that is a git checkout is still indexed");

    // The launcher's kind filter: kind:<word> in the query line, alone = newest of that kind.
    const QueryServer server(db, -1);
    const std::string pdfs = server.answer("query 20 kind:pdf");
    check(pdfs.find("Quarterly Report.pdf") != std::string::npos && pdfs.find("Résumé Élan.pdf") != std::string::npos
              && pdfs.find(".txt") == std::string::npos,
          "kind alone lists only that kind");
    const std::string pdfReport = server.answer("query 20 report kind:pdf");
    check(pdfReport.find("Quarterly Report.pdf") != std::string::npos && pdfReport.find("report-draft.txt") == std::string::npos
              && pdfReport.find("/Report\t") == std::string::npos,
          "kind filters a text query");
    check(server.answer("query 20 kind:folder notes").find("/notes\t") != std::string::npos, "kind:folder finds folders");
    check(server.answer("query 20 kind:nope report") == "\n", "an unknown kind answers nothing");

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

  // The content worker pauses only for a discharging battery under the limit.
  const fs::path supplies = root / "power_supply";
  const auto supply = [&](const char* name, const char* type, const char* status, const char* capacity) {
    fs::create_directories(supplies / name);
    std::ofstream(supplies / name / "type") << type << "\n";
    std::ofstream(supplies / name / "status") << status << "\n";
    std::ofstream(supplies / name / "capacity") << capacity << "\n";
  };
  supply("AC", "Mains", "Unknown", "0");
  supply("BAT0", "Battery", "Charging", "10");
  check(!batteryLow(20, supplies), "a charging battery does not pause");
  supply("BAT0", "Battery", "Discharging", "35");
  check(!batteryLow(20, supplies), "a discharging battery above the limit does not pause");
  supply("BAT0", "Battery", "Discharging", "12");
  check(batteryLow(20, supplies), "a discharging battery below the limit pauses");

  fs::remove_all(root);
  std::printf(g_failures == 0 ? "PASS\n" : "%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
