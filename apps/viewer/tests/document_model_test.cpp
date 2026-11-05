#include "document_model.h"

#include <print>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "document_model_test: {}", what);
      ++g_failures;
    }
  }

  std::vector<viewer::RectF> hits(std::size_t n) { return std::vector<viewer::RectF>(n, viewer::RectF{1, 2, 3, 4}); }
} // namespace

int main() {
  using viewer::DocumentModel;

  // Search walks hits in page order, skips pages without hits and wraps at the end.
  {
    DocumentModel model;
    model.reset(4, 600.0F, 800.0F);
    model.startSearch("word");
    expect(model.countText().empty(), "no count before the first hit while scanning");
    for (std::size_t page : {0UZ, 1UZ, 2UZ, 3UZ}) {
      expect(model.nextPageToScan() == page, "pages scan in order");
      model.setHits(page, hits(page == 1 ? 2 : page == 3 ? 1 : 0));
    }
    expect(model.searchDone() && model.hitCount() == 3, "three hits after the scan");
    expect(model.nextHit(0) && model.currentOrdinal() == 1 && model.currentHit()->page == 1, "first hit on page 2");
    expect(model.countText() == "1 of 3", "count text: " + model.countText());
    expect(model.nextHit(0) && model.currentOrdinal() == 2, "second hit on the same page");
    expect(model.nextHit(0) && model.currentHit()->page == 3, "third hit on page 4");
    expect(model.nextHit(0) && model.currentOrdinal() == 1, "next after the last wraps to the first");
    expect(model.previousHit(0) && model.currentOrdinal() == 3, "previous before the first wraps to the last");
  }

  // The first hit starts at the page in view, not at page 1.
  {
    DocumentModel model;
    model.reset(3, 600.0F, 800.0F);
    model.startSearch("word");
    for (std::size_t page = 0; page < 3; ++page) {
      model.setHits(page, hits(1));
    }
    expect(model.nextHit(2) && model.currentHit()->page == 2, "next from the page in view");
  }

  // A reply for a page out of order (an older search) is ignored; no hits ends as "No results".
  {
    DocumentModel model;
    model.reset(2, 600.0F, 800.0F);
    model.startSearch("absent");
    model.setHits(1, hits(5));
    expect(model.hitCount() == 0, "out-of-order page ignored");
    model.setHits(0, {});
    model.setHits(1, {});
    expect(model.countText() == "No results", "no results: " + model.countText());
    expect(!model.nextHit(0), "no hit to move to");
  }

  // Fit-width zoom follows the viewport; pages stack with the gap; pageAt finds the page under a y.
  {
    DocumentModel model;
    model.reset(3, 500.0F, 1000.0F);
    model.setViewportWidth(1000.0F);
    expect(model.zoom() == 2.0F, "fit width zoom");
    const auto second = model.pageBox(1);
    expect(second.y0 == 2000.0F + DocumentModel::kPageGap, "second page top");
    expect(model.pageAt(second.y0 + 1.0F) == 1 && model.pageAt(1e9F) == 2, "page under y");
    model.zoomBy(0.5F);
    model.setViewportWidth(2000.0F);
    expect(model.zoom() == 1.0F && !model.fitsWidth(), "a user zoom stops following the viewport");
  }

  return g_failures == 0 ? 0 : 1;
}
