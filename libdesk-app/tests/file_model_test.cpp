#include "core/deferred_call.h"
#include "files/directory_watcher.h"
#include "files/file_model.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "file_model_test: {}", what);
      ++g_failures;
    }
  }

  // The app loop in miniature: the DeferredCall wake fd and the watcher, until done() or 3 s.
  template <typename F> bool pump(DirectoryWatcher& watcher, F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::vector<pollfd> fds{{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0}};
      const std::size_t start = watcher.addPollFds(fds);
      ::poll(fds.data(), fds.size(), std::max(watcher.pollTimeoutMs(), 10));
      DeferredCall::drainWakeFd();
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
      watcher.dispatch(fds, start);
    }
    return true;
  }

  std::vector<std::string> names(const FileModelView& view) {
    std::vector<std::string> out;
    for (const auto& row : view.rows()) {
      out.push_back(row->name);
    }
    return out;
  }

  std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
      out += (out.empty() ? "" : ",") + s;
    }
    return out;
  }

  void write(const fs::path& path, std::size_t bytes) { std::ofstream(path) << std::string(bytes, 'x'); }
} // namespace

int main() {
  char tmpl[] = "/tmp/desk-file-model-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const fs::path dir = root / "dir";
  const fs::path other = root / "other";
  fs::create_directories(dir / "zdir");
  fs::create_directories(other);
  write(dir / "b.txt", 30);
  write(dir / "a10.txt", 10);
  write(dir / "a2.txt", 20);
  write(dir / ".hidden", 1);

  DirectoryWatcher watcher;
  auto model = FileModel::open(dir, watcher);
  expect(FileModel::open(dir / "." , watcher) == model, "one model per directory per process");
  expect(pump(watcher, [&] { return model->status() == FileModel::Status::Ready; }), "the listing finishes");

  FileModelView view(model);
  expect(joined(names(view)) == "zdir,a2.txt,a10.txt,b.txt", "folders first, then natural name order: " + joined(names(view)));
  view.setShowHidden(true);
  expect(names(view).size() == 5 && names(view)[1] == ".hidden", "show hidden adds the dot file: " + joined(names(view)));
  view.setShowHidden(false);
  view.setSort({.key = FileSortKey::Size, .descending = true});
  expect(joined(names(view)) == "zdir,b.txt,a2.txt,a10.txt", "size descending keeps folders first: " + joined(names(view)));
  view.setSort({});
  view.setFilter("A1");
  expect(joined(names(view)) == "a10.txt", "the filter is a case-insensitive substring: " + joined(names(view)));
  view.setFilter("");

  std::vector<FileModel::Change> changes;
  auto token = model->subscribe([&](const FileModel::Change& c) { changes.push_back(c); });
  const auto saw = [&](FileModel::Change::Type type, const std::string& name) {
    return pump(watcher, [&] {
      return std::ranges::any_of(changes, [&](const auto& c) { return c.type == type && c.name == name; });
    });
  };

  write(dir / "new.txt", 5);
  expect(saw(FileModel::Change::Type::Inserted, "new.txt"), "a created file is inserted");
  expect(model->find("new.txt") != nullptr && model->find("new.txt")->size == 5, "the inserted entry has its size");

  write(dir / "new.txt", 50);
  expect(saw(FileModel::Change::Type::Updated, "new.txt"), "a rewritten file is updated");
  expect(pump(watcher, [&] { return model->find("new.txt")->size == 50; }), "the update carries the new size");

  fs::rename(dir / "new.txt", dir / "renamed.txt");
  expect(saw(FileModel::Change::Type::Renamed, "new.txt"), "a rename inside the folder is one rename");
  expect(model->find("new.txt") == nullptr && model->find("renamed.txt") != nullptr, "the rename moves the entry");

  fs::rename(dir / "renamed.txt", other / "renamed.txt");
  expect(saw(FileModel::Change::Type::Removed, "renamed.txt"), "a move out of the folder removes the entry");

  fs::rename(other / "renamed.txt", dir / "back.txt");
  expect(saw(FileModel::Change::Type::Inserted, "back.txt"), "a move into the folder inserts the entry");

  fs::remove(dir / "b.txt");
  expect(saw(FileModel::Change::Type::Removed, "b.txt"), "a deleted file is removed");
  expect(joined(names(view)) == "zdir,a2.txt,a10.txt,back.txt", "the view follows the model: " + joined(names(view)));

  fs::remove_all(dir);
  expect(pump(watcher, [&] { return model->status() == FileModel::Status::Missing; }), "a deleted folder reads as missing");
  expect(model->entries().empty(), "a missing folder has no entries");

  auto missing = FileModel::open(root / "nope", watcher);
  expect(pump(watcher, [&] { return missing->status() == FileModel::Status::Missing; }), "a folder that never existed is missing");

  fs::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
