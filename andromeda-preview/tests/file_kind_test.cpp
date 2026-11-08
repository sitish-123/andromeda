#include "../src/file_kind.h"

#include <cstdio>

int main() {
  using preview::FileKind;
  using preview::kindForContentType;
  int failures = 0;
  auto expect = [&](const char* type, FileKind want) {
    if (kindForContentType(type) != want) {
      std::fprintf(stderr, "FAIL %s\n", type);
      ++failures;
    }
  };
  expect("application/pdf", FileKind::Pdf);
  expect("image/png", FileKind::Image);
  expect("image/svg+xml", FileKind::Image);
  expect("text/x-python", FileKind::Text);
  expect("application/x-shellscript", FileKind::Text);
  expect("video/mp4", FileKind::Video);
  expect("audio/flac", FileKind::Audio);
  expect("inode/directory", FileKind::Folder);
  expect("application/zip", FileKind::Archive);
  expect("application/octet-stream", FileKind::Other);
  return failures == 0 ? 0 : 1;
}
