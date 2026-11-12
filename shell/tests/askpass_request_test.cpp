#include "ipc/ipc_poll_source.h"
#include "ipc/ipc_service.h"
#include "shell/polkit/askpass_broker.h"
#include "shell/polkit/askpass_request.h"
#include "tests/test_check.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <format>
#include <functional>
#include <fstream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  fs::path makeTempDir() {
    std::string pattern = (fs::temp_directory_path() / "andromeda-askpass-XXXXXX").string();
    return fs::path(::mkdtemp(pattern.data()));
  }

  void writeFile(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  }

  // askpass `pid` -> sudo 200 (effective uid `sudoEuid`) running `sudoArgv` -> requester 100 "claude" in `cwd`.
  void fakeChain(const fs::path& root, pid_t pid, std::string_view sudoArgv, const fs::path& cwd, long sudoEuid = 0,
                 long sudoRuid = ::getuid()) {
    const fs::path peer = root / std::to_string(pid);
    writeFile(peer / "stat", std::to_string(pid) + " (andromeda-askp) S 200 1 1");
    writeFile(peer / "status", "Name:\tandromeda-askp\nTracerPid:\t0\n");
    writeFile(root / "200" / "stat", "200 (sudo) S 100 1 1");
    writeFile(root / "200" / "status", std::format("Name:\tsudo\nUid:\t{}\t{}\t{}\t{}\n", sudoRuid, sudoEuid, sudoEuid, sudoEuid));
    writeFile(root / "200" / "comm", "sudo\n");
    writeFile(root / "200" / "cmdline", sudoArgv);
    writeFile(root / "100" / "stat", "100 (a) b) S 1 1 1");
    writeFile(root / "100" / "comm", "claude\n");
    if (!fs::exists(root / "100" / "cwd")) {
      fs::create_directory_symlink(cwd, root / "100" / "cwd");
    }
  }

  enum class PeerReply { Ready, ReadyThenExit, PartialReady };

  // A stand-in for the helper: sends a request with forged text, answers "verify" as `reply` says, and copies
  // everything the shell sent to `out`.
  pid_t spawnPeer(const std::string& socketPath, int out, PeerReply reply = PeerReply::Ready) {
    const pid_t pid = ::fork();
    if (pid != 0) {
      return pid;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::_exit(2);
    }
    const std::string_view request = "askpass sudo -A rm -rf /home\n";
    (void)!::write(fd, request.data(), request.size());
    std::string got;
    char c = 0;
    while (::read(fd, &c, 1) == 1) {
      got += c;
      if (c == '\n') {
        break;
      }
    }
    if (got == "verify\n" && reply == PeerReply::PartialReady) {
      (void)!::write(fd, "rea", 3);
    } else if (got == "verify\n") {
      (void)!::write(fd, "ready\n", 6);
      if (reply == PeerReply::ReadyThenExit) {
        ::_exit(0);
      }
    }
    char buf[256];
    for (ssize_t n = 0; (n = ::read(fd, buf, sizeof(buf))) > 0;) {
      got.append(buf, static_cast<std::size_t>(n));
    }
    (void)!::write(out, got.data(), got.size());
    ::_exit(0);
  }

  int connectTo(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    TEST_CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
  }

  std::string readAll(int fd) {
    std::string out;
    char buf[256];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  void pollOnce(std::vector<PollSource*> sources) {
    std::vector<pollfd> fds;
    std::vector<std::size_t> starts;
    for (PollSource* source : sources) {
      starts.push_back(source->addPollFds(fds));
    }
    TEST_CHECK(::poll(fds.data(), fds.size(), 200) >= 0);
    for (std::size_t i = 0; i < sources.size(); ++i) {
      sources[i]->dispatch(fds, starts[i]);
    }
  }

} // namespace

int main() {
  using namespace std::string_literals;
  const fs::path tmp = makeTempDir();
  const fs::path cwd = tmp / "work dir";
  fs::create_directories(cwd);

  {
    const fs::path root = tmp / "proc1";
    fakeChain(root, 300, "sudo\0-A\0-k\0pacman\0-S\0foo bar\0it's\0a\nb\0"s, cwd);
    const auto request = readAskpassRequest(root, 300);
    TEST_CHECK(request.has_value());
    TEST_CHECK(request->command == R"(sudo -A -k pacman -S 'foo bar' 'it'\''s' 'a\x0ab')");
    TEST_CHECK(request->requester == "claude");
    TEST_CHECK(request->requesterPid == 100);
    TEST_CHECK(request->cwd == cwd.string());
    TEST_CHECK(!readAskpassRequest(root, 999).has_value());
  }

  {
    const fs::path root = tmp / "proc-unicode";
    fakeChain(root, 300, "sudo\0a\xe2\x80\xae" "b\0a\xe2\x80\x8b" "b\0a\xc2\x85" "b\0a\xff" "b\0caf\xc3\xa9\0"s, cwd);
    const auto request = readAskpassRequest(root, 300);
    TEST_CHECK(request.has_value());
    TEST_CHECK(request->command == R"(sudo 'a\u{202e}b' 'a\u{200b}b' 'a\u{0085}b' 'a\xffb' café)");
  }

  {
    const fs::path root = tmp / "proc2";
    std::string longArg;
    for (int i = 0; i < 400; ++i) {
      longArg += "é";
    }
    fakeChain(root, 300, "sudo\0"s + longArg + '\0', cwd);
    const auto request = readAskpassRequest(root, 300);
    TEST_CHECK(request.has_value());
    TEST_CHECK(request->command == "sudo " + longArg.substr(0, 295 * 2) + "…");
  }

  // A non-dumpable process keeps a readable status: the recheck after the handshake works on the real /proc. A
  // dumpable one, which the user could have injected code into, is refused.
  for (const bool dumpable : {false, true}) {
    int ready[2];
    int hold[2];
    TEST_CHECK(::pipe(ready) == 0 && ::pipe(hold) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
      ::close(hold[1]);
      ::prctl(PR_SET_DUMPABLE, dumpable ? 1 : 0);
      (void)!::write(ready[1], "x", 1);
      char c = 0;
      (void)!::read(hold[0], &c, 1);
      ::_exit(0);
    }
    char c = 0;
    TEST_CHECK(::read(ready[0], &c, 1) == 1);
    const auto refused = recheckAskpassCaller(AskpassProc{}, child, ::getpid());
    TEST_CHECK(dumpable ? refused == "the caller is dumpable" : !refused.has_value());
    ::close(hold[1]);
    TEST_CHECK(::waitpid(child, nullptr, 0) == child);
  }

  // Over the IPC socket, with forked peers: the request comes from /proc of the SO_PEERCRED pid, never from the
  // client's text, and only after the handshake.
  {
    TEST_CHECK(::setenv("XDG_RUNTIME_DIR", tmp.c_str(), 1) == 0);
    TEST_CHECK(::setenv("WAYLAND_DISPLAY", "askpass-test", 1) == 0);
    IpcService ipc;
    TEST_CHECK(ipc.start());
    IpcPollSource ipcSource(ipc);
    const fs::path root = tmp / "proc3";
    // The fake tree belongs to this user, so it stands for a non-dumpable process here.
    const AskpassProc proc{.procRoot = root, .helperGid = ::getegid(), .uid = ::getuid(), .nonDumpableOwner = ::getuid()};
    const auto openPidfd = [](pid_t pid) {
      return pid == 200 ? static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0)) : -1;
    };
    AskpassBroker broker(proc, openPidfd);
    AskpassProc otherGroupProc = proc;
    otherGroupProc.helperGid = ::getegid() + 1;
    AskpassBroker otherGroup(otherGroupProc, openPidfd);
    AskpassProc dumpableProc = proc;
    dumpableProc.nonDumpableOwner = 0;
    AskpassBroker dumpable(dumpableProc, openPidfd);
    AskpassBroker* active = &broker;
    int changes = 0;
    broker.setStateCallback([&changes] { ++changes; });
    ipc.setAskpassHandler([&active](int fd, pid_t pid) { return active->begin(fd, pid); });
    const auto pump = [&](const std::function<bool()>& done) {
      for (int i = 0; i < 50 && !done(); ++i) {
        pollOnce({&ipcSource, active});
      }
      TEST_CHECK(done());
    };
    // Runs one peer to its end and returns what the shell sent it.
    const auto refusedWith = [&](long sudoEuid, long sudoRuid = ::getuid()) {
      int out[2];
      TEST_CHECK(::pipe(out) == 0);
      const pid_t child = spawnPeer(ipc.socketPath(), out[1]);
      ::close(out[1]);
      fs::remove_all(root);
      fakeChain(root, child, "sudo\0-A\0true\0"s, cwd, sudoEuid, sudoRuid);
      int status = 0;
      bool exited = false;
      pump([&] { return exited = exited || ::waitpid(child, &status, WNOHANG) == child; });
      std::string got = readAll(out[0]);
      ::close(out[0]);
      TEST_CHECK(!active->busy());
      return got;
    };

    // A process that connected without the helper's setgid group, such as one that later execs the helper.
    active = &otherGroup;
    TEST_CHECK(refusedWith(0) == "error: the caller is not andromeda-askpass\n");
    active = &dumpable;
    TEST_CHECK(refusedWith(0) == "error: the caller is dumpable\n");
    active = &broker;
    TEST_CHECK(refusedWith(::getuid()) == "error: the caller was not started by sudo\n");
    TEST_CHECK(refusedWith(0, ::getuid() + 1) == "error: the caller was not started by sudo\n");

    {
      int out[2];
      TEST_CHECK(::pipe(out) == 0);
      const pid_t child = spawnPeer(ipc.socketPath(), out[1]);
      ::close(out[1]);
      fs::remove_all(root);
      fakeChain(root, child, "sudo\0-A\0true\0"s, cwd);
      pump([&] { return broker.hasPendingRequest(); });
      TEST_CHECK(changes == 1);
      TEST_CHECK(broker.pendingRequest().command == "sudo -A true");
      TEST_CHECK(broker.pendingRequest().requester == "claude");

      const int second = connectTo(ipc.socketPath());
      TEST_CHECK(::write(second, "askpass\n", 8) == 8);
      pollOnce({&ipcSource, &broker});
      TEST_CHECK(readAll(second).starts_with("error: "));
      ::close(second);

      broker.submitResponse("s3cret pw");
      TEST_CHECK(!broker.hasPendingRequest());
      TEST_CHECK(changes == 2);
      TEST_CHECK(readAll(out[0]) == "verify\npassword\ns3cret pw\n");
      ::close(out[0]);
      TEST_CHECK(::waitpid(child, nullptr, 0) == child);
    }

    // A tracer attached while the helper was dumpable.
    {
      int out[2];
      TEST_CHECK(::pipe(out) == 0);
      const pid_t child = spawnPeer(ipc.socketPath(), out[1]);
      ::close(out[1]);
      fs::remove_all(root);
      fakeChain(root, child, "sudo\0-A\0true\0"s, cwd);
      pump([&] { return broker.busy(); });
      writeFile(root / std::to_string(child) / "status", "Name:\tandromeda-askp\nTracerPid:\t4242\n");
      pump([&] { return !broker.busy(); });
      TEST_CHECK(readAll(out[0]) == "verify\nerror: the caller is traced\n");
      ::close(out[0]);
      TEST_CHECK(::waitpid(child, nullptr, 0) == child);
      TEST_CHECK(changes == 2);
    }

    // The peer died after "ready": its pidfd says so.
    {
      int out[2];
      TEST_CHECK(::pipe(out) == 0);
      const pid_t child = spawnPeer(ipc.socketPath(), out[1], PeerReply::ReadyThenExit);
      ::close(out[1]);
      ::close(out[0]);
      fs::remove_all(root);
      fakeChain(root, child, "sudo\0-A\0true\0"s, cwd);
      pump([&] { return broker.busy(); });
      TEST_CHECK(::waitpid(child, nullptr, 0) == child);
      pump([&] { return !broker.busy(); });
      TEST_CHECK(changes == 2);
    }

    // A peer that never finishes "ready" loses the one slot after 2 s, and the next request gets it.
    {
      int out[2];
      TEST_CHECK(::pipe(out) == 0);
      const pid_t child = spawnPeer(ipc.socketPath(), out[1], PeerReply::PartialReady);
      ::close(out[1]);
      fs::remove_all(root);
      fakeChain(root, child, "sudo\0-A\0true\0"s, cwd);
      pump([&] { return broker.busy(); });
      pump([&] { return !broker.busy(); });
      TEST_CHECK(readAll(out[0]) == "verify\nerror: no ready in time\n");
      ::close(out[0]);
      TEST_CHECK(::waitpid(child, nullptr, 0) == child);

      TEST_CHECK(::pipe(out) == 0);
      const pid_t next = spawnPeer(ipc.socketPath(), out[1]);
      ::close(out[1]);
      fs::remove_all(root);
      fakeChain(root, next, "sudo\0-A\0true\0"s, cwd);
      pump([&] { return broker.hasPendingRequest(); });
      broker.cancelRequest();
      TEST_CHECK(readAll(out[0]) == "verify\ncancel\n");
      ::close(out[0]);
      TEST_CHECK(::waitpid(next, nullptr, 0) == next);
    }
  }

  fs::remove_all(tmp);
  return 0;
}
