#include "check.h"
#include "protocols/crash_capture.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

  volatile int* gNull = nullptr;

  [[gnu::noinline]] int faultInner(int depth) { return *gNull + depth; }
  [[gnu::noinline]] int faultMiddle(int depth) { return faultInner(depth + 1) + 1; }
  [[gnu::noinline]] int faultOuter(int depth) { return faultMiddle(depth + 1) + 1; }

  [[noreturn]] void crashingChild(const std::string& stateHome) {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    setenv("XDG_STATE_HOME", stateHome.c_str(), 1);
    andromeda::crash::install("crash-test", "9.8.7 (g1234abc)");
    for (int i = 0; i < 250; ++i) {
      andromeda::crash::record('I', "test", "line " + std::to_string(i));
    }
    andromeda::crash::record('E', "render", "last line before the fault");
    _exit(faultOuter(0));
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  std::size_t countOf(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
      ++count;
    }
    return count;
  }

  [[noreturn]] void twoThreadsCrashingChild(const std::string& stateHome) {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    setenv("XDG_STATE_HOME", stateHome.c_str(), 1);
    andromeda::crash::install("crash-test", "1");
    for (int i = 0; i < 200; ++i) {
      andromeda::crash::record('I', "test", "a log line long enough to make the report take a while to write");
    }
    std::atomic<int> ready{0};
    const auto fault = [&ready] {
      andromeda::crash::installThreadStack();
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      _exit(faultOuter(0));
    };
    std::thread first(fault);
    std::thread second(fault);
    first.join();
    second.join();
    _exit(0);
  }

} // namespace

ANDROMEDA_TEST(twoThreadsCrashingAtOnceWriteOneCleanReport) {
  for (int round = 0; round < 20; ++round) {
    char pattern[] = "/tmp/crash-capture-XXXXXX";
    const std::string stateHome = mkdtemp(pattern);
    const pid_t pid = fork();
    if (pid == 0) {
      twoThreadsCrashingChild(stateHome);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(stateHome + "/andromeda/crashes")) {
      ++files;
      const std::string report = readFile(entry.path());
      CHECK_EQ(countOf(report, "component: "), std::size_t{1});
      CHECK_EQ(countOf(report, "backtrace:\n"), std::size_t{1});
      CHECK(report.ends_with("take a while to write\n"));
    }
    CHECK_EQ(files, 1);
    std::filesystem::remove_all(stateHome);
  }
}

ANDROMEDA_TEST(handlerInstallsWithoutAWritableCrashDirectory) {
  const pid_t pid = fork();
  if (pid == 0) {
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    setenv("XDG_STATE_HOME", "/proc/no-such-dir", 1);
    setenv("HOME", "/proc/no-such-home", 1);
    andromeda::crash::install("crash-test", "1");
    struct sigaction current{};
    sigaction(SIGSEGV, nullptr, &current);
    if ((current.sa_flags & SA_SIGINFO) == 0) {
      _exit(3);
    }
    _exit(faultOuter(0));
  }
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WIFSIGNALED(status) ? WTERMSIG(status) : -WEXITSTATUS(status), SIGSEGV);
}

ANDROMEDA_TEST(fatalSignalWritesReportAndStillDies) {
  char pattern[] = "/tmp/crash-capture-XXXXXX";
  const std::string stateHome = mkdtemp(pattern);

  const pid_t pid = fork();
  if (pid == 0) {
    crashingChild(stateHome);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGSEGV);

  const std::filesystem::path dir = stateHome + "/andromeda/crashes";
  std::string report;
  std::string name;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    name = entry.path().filename().string();
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    report = text.str();
  }
  CHECK(name.starts_with("crash-test-"));
  CHECK(name.ends_with("-" + std::to_string(pid) + ".txt"));
  CHECK(report.contains("component: crash-test\n"));
  CHECK(report.contains("version: 9.8.7 (g1234abc)\n"));
  CHECK(report.contains("signal: SIGSEGV (11)\n"));
  CHECK(report.contains("pid: " + std::to_string(pid) + "\n"));

  int frames = 0;
  bool firstFrameInTest = false;
  std::istringstream lines(report);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("#") && line.contains("+0x")) {
      if (frames == 0) {
        firstFrameInTest = line.contains("crash-capture-test+");
      }
      ++frames;
    }
  }
  CHECK(frames >= 3);
  CHECK(firstFrameInTest);

  const auto logAt = report.find("log:\n");
  CHECK(logAt != std::string::npos);
  CHECK(report.contains("E [render] last line before the fault\n"));
  CHECK(report.contains("I [test] line 249\n"));
  // The ring keeps only the last 200 lines: 51 of the 251 are gone.
  CHECK(!report.contains("I [test] line 50\n"));
  CHECK(report.contains("I [test] line 51\n"));

  std::filesystem::remove_all(stateHome);
}

int main() { return RUN_TESTS(); }
