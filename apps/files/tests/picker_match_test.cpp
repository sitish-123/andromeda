#include "picker_portal.h"

#include <print>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "picker_match_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  // Chromium's "All Supported Types" for accept="image/*": case-insensitive bracket globs.
  const PickerPortal::Filter chromium{.name = "All Supported Types",
                                      .patterns = {{0, "*.[pP][nN][gG]"}, {0, "*.[jJ][pP][gG]"}}};
  expect(PickerPortal::matches(chromium, "Screenshot_2026-10-09.png", ""), "bracket glob matches a lower-case name");
  expect(PickerPortal::matches(chromium, "IMG_0412.JPG", ""), "bracket glob matches an upper-case name");
  expect(!PickerPortal::matches(chromium, "notes.txt", ""), "bracket glob rejects another type");

  const PickerPortal::Filter mime{.name = "Images", .patterns = {{1, "image/*"}}};
  expect(PickerPortal::matches(mime, "a.png", "image/png"), "MIME wildcard matches");
  expect(!PickerPortal::matches(mime, "a.mp4", "video/mp4"), "MIME wildcard rejects video");
  return g_failures == 0 ? 0 : 1;
}
