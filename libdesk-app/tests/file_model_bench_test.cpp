#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_model.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "file_model_bench_test: {}", what);
      ++g_failures;
    }
  }

  template <typename F> bool pump(DirectoryWatcher& watcher, F done) {
    const auto until = Clock::now() + std::chrono::seconds(30);
    while (!done()) {
      if (Clock::now() > until) {
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

  double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
} // namespace

int main() {
  constexpr int kFiles = 100'000;
  char tmpl[] = "/tmp/desk-file-bench-XXXXXX";
  const fs::path dir = ::mkdtemp(tmpl);
  for (int i = 0; i < kFiles; ++i) {
    const int fd = ::open((dir / ("file " + std::to_string(i) + ".txt")).c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) {
      ::close(fd);
    }
  }

  DirectoryWatcher watcher;
  const auto start = Clock::now();
  auto model = FileModel::open(dir, watcher);
  FileModelView view(model);
  Clock::time_point firstNames{};
  bool prioritized = false;
  view.setChangedCallback([&] {
    if (!prioritized && !view.rows().empty()) {
      firstNames = Clock::now();
      prioritized = true;
      model->prioritizeStat(std::span(view.rows()).first(std::min<std::size_t>(view.rows().size(), 60)));
    }
  });
  expect(pump(watcher, [&] { return model->status() == FileModel::Status::Ready && !model->statPending(); }),
         "the listing and its stat finish");
  const auto settled = Clock::now();

  expect(view.rows().size() == kFiles, "every file is a row: " + std::to_string(view.rows().size()));
  expect(view.rows().size() > 10 && view.rows()[2]->name == "file 2.txt" && view.rows()[10]->name == "file 10.txt",
         "natural name order");
  expect(std::ranges::none_of(model->entries(), [](const auto& e) { return e->pendingStat; }),
         "every entry has its stat");
  std::println("file_model_bench_test: {} files: first names {:.1f} ms, full stat {:.1f} ms", kFiles,
               ms(firstNames - start), ms(settled - start));

  model.reset();
  fs::remove_all(dir);
  return g_failures == 0 ? 0 : 1;
}
