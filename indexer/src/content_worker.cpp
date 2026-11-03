#include "content_worker.h"

#include "extract.h"
#include "index_db.h"

#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

namespace andromeda::index {

  namespace {
    constexpr int kIoprioClassIdle = 3;
    constexpr int kIoprioClassShift = 13;
    constexpr int kIoprioWhoProcess = 1; // with id 0: the calling thread
    constexpr auto kIdleRecheck = std::chrono::minutes(10);
    constexpr auto kBatteryRecheck = std::chrono::minutes(1);

    void lowerPriority() {
      const sched_param param{};
      pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
      syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, kIoprioClassIdle << kIoprioClassShift);
    }

    constexpr int kExtractTimeoutMs = 30'000;

    // The andromeda-index-extract process for one pass: paths go in on a pipe, text comes back framed by its size.
    class Extractor {
    public:
      Extractor() {
        std::error_code ec;
        const auto helper = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path() / "andromeda-index-extract";
        int toChild[2] = {-1, -1};
        int fromChild[2] = {-1, -1};
        if (ec || pipe2(toChild, O_CLOEXEC) != 0 || pipe2(fromChild, O_CLOEXEC) != 0) {
          return;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
        const std::string path = helper.string();
        char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
        const bool started = posix_spawn(&m_pid, path.c_str(), &actions, nullptr, argv, environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
        close(toChild[0]);
        close(fromChild[1]);
        if (!started) {
          close(toChild[1]);
          close(fromChild[0]);
          m_pid = -1;
          return;
        }
        m_in = toChild[1];
        m_out = fromChild[0];
      }

      ~Extractor() {
        if (m_in >= 0) {
          close(m_in);
        }
        if (m_out >= 0) {
          close(m_out);
        }
        if (m_pid > 0) {
          kill(m_pid, SIGKILL);
          waitpid(m_pid, nullptr, 0);
        }
      }

      Extractor(const Extractor&) = delete;
      Extractor& operator=(const Extractor&) = delete;

      [[nodiscard]] bool ok() const { return m_pid > 0; }

      // The text of path; the outer nullopt when the helper crashed or took longer than kExtractTimeoutMs.
      std::optional<std::optional<std::string>> extract(const std::string& path) {
        const std::string line = path + "\n";
        if (write(m_in, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
          return std::nullopt;
        }
        std::string header;
        while (!header.ends_with('\n')) {
          char c = 0;
          if (!readExact(&c, 1)) {
            return std::nullopt;
          }
          header += c;
        }
        if (header == "-\n") {
          return std::optional<std::string>();
        }
        std::string text(static_cast<std::size_t>(std::atoll(header.c_str())), '\0');
        if (!readExact(text.data(), text.size())) {
          return std::nullopt;
        }
        return std::optional(std::move(text));
      }

    private:
      bool readExact(char* data, std::size_t size) {
        while (size > 0) {
          pollfd p{.fd = m_out, .events = POLLIN, .revents = 0};
          if (poll(&p, 1, kExtractTimeoutMs) <= 0) {
            return false;
          }
          const ssize_t n = read(m_out, data, size);
          if (n <= 0) {
            return false;
          }
          data += n;
          size -= static_cast<std::size_t>(n);
        }
        return true;
      }

      pid_t m_pid = -1;
      int m_in = -1;
      int m_out = -1;
    };

    std::string firstLine(const std::filesystem::path& file) {
      std::ifstream in(file);
      std::string line;
      std::getline(in, line);
      return line;
    }
  } // namespace

  bool batteryLow(int percent, const std::filesystem::path& powerSupplies) {
    std::error_code ec;
    for (const auto& supply : std::filesystem::directory_iterator(powerSupplies, ec)) {
      if (firstLine(supply.path() / "type") != "Battery" || firstLine(supply.path() / "status") != "Discharging") {
        continue;
      }
      const std::string capacity = firstLine(supply.path() / "capacity");
      if (!capacity.empty() && std::atoi(capacity.c_str()) < percent) {
        return true;
      }
    }
    return false;
  }

  ContentWorker::ContentWorker(std::filesystem::path dbFile) : m_dbFile(std::move(dbFile)), m_thread([this] { run(); }) {}

  ContentWorker::~ContentWorker() {
    {
      const std::lock_guard lock(m_mutex);
      m_stop = true;
    }
    m_cv.notify_one();
    m_thread.join();
  }

  void ContentWorker::wake() {
    {
      const std::lock_guard lock(m_mutex);
      m_woken = true;
    }
    m_cv.notify_one();
  }

  void ContentWorker::run() {
    lowerPriority();
    IndexDb db(m_dbFile);
    if (!db.ok()) {
      return;
    }
    db.failInterrupted();
    while (true) {
      {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, kIdleRecheck, [this] { return m_woken || m_stop; });
        if (m_stop) {
          return;
        }
        m_woken = false;
      }
      const auto start = std::chrono::steady_clock::now();
      std::size_t done = 0;
      std::optional<Extractor> extractor;
      for (const ContentTask& task : db.contentTasks()) {
        while (batteryLow(kLowBattery)) {
          std::unique_lock lock(m_mutex);
          if (m_cv.wait_for(lock, kBatteryRecheck, [this] { return m_stop; })) {
            return;
          }
        }
        {
          const std::lock_guard lock(m_mutex);
          if (m_stop) {
            return;
          }
        }
        if (!needsHelper(documentKind(task.path))) {
          db.storeContent(task.id, task.mtime, extractPlain(task.path));
          ++done;
          continue;
        }
        if (!extractor || !extractor->ok()) {
          extractor.emplace();
          if (!extractor->ok()) {
            std::fprintf(stderr, "content: andromeda-index-extract did not start\n");
            break;
          }
        }
        // The helper reads one path per line.
        if (task.path.find('\n') != std::string::npos) {
          db.storeContent(task.id, task.mtime, std::nullopt);
          continue;
        }
        db.markExtracting(task.id, task.mtime);
        const auto text = extractor->extract(task.path);
        // A crash or a hang: the file is recorded as unreadable and the next file gets a new helper.
        db.storeContent(task.id, task.mtime, text ? *text : std::nullopt);
        if (!text) {
          extractor.reset();
        }
        ++done;
      }
      extractor.reset();
      if (done > 0) {
        db.releaseMemory();
        malloc_trim(0);
        std::fprintf(stderr, "content: %zu files read in %lld ms\n", done,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - start)
                                                .count()));
      }
    }
  }

} // namespace andromeda::index
