#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>

// What the askpass dialog shows. Read from /proc of the connected askpass process, never from its own text:
// its parent (sudo) runs `command`, and the parent's parent is the requester.
struct AskpassRequest {
  // The parent's argv, shell-quoted where needed, control bytes escaped, cut to kAskpassCommandMaxChars.
  std::string command;
  std::string requester;
  pid_t requesterPid = 0;
  // Empty when the requester's cwd cannot be read.
  std::string cwd;
};

// UTF-8 code points; a longer command ends with an ellipsis.
inline constexpr std::size_t kAskpassCommandMaxChars = 300;

// Nullopt when the parent of `askpassPid` or its argv cannot be read.
[[nodiscard]] std::optional<AskpassRequest> readAskpassRequest(const std::filesystem::path& procRoot, pid_t askpassPid);

// The gid of group `andromeda-askpass`, looked up once; nullopt when the group does not exist.
[[nodiscard]] std::optional<gid_t> askpassGroup();

// Where the checks read from; tests point it at a fake tree.
struct AskpassProc {
  std::filesystem::path procRoot = "/proc";
  // The installed helper is setgid to this group, so only it connects with this effective gid. Nullopt refuses all.
  std::optional<gid_t> helperGid = askpassGroup();
  uid_t uid = ::getuid();
  // The kernel gives the files in /proc/<pid> of a non-dumpable process to root (the directory keeps its owner).
  uid_t nonDumpableOwner = 0;
};

struct AskpassCaller {
  pid_t parentPid = 0;
  AskpassRequest request;
};

// Before the handshake: the peer runs the installed helper (its SO_PEERCRED gid is the helper's group), and its
// parent is sudo started by this user.
[[nodiscard]] std::expected<AskpassCaller, std::string>
verifyAskpassCaller(const AskpassProc& proc, pid_t peerPid, gid_t peerGid);

// At connect and after "ready": the helper is non-dumpable, no tracer is attached and the parent is unchanged.
// Returns the reason it fails, or nullopt.
[[nodiscard]] std::optional<std::string> recheckAskpassCaller(const AskpassProc& proc, pid_t peerPid, pid_t parentPid);
