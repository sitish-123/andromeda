#include "shell/polkit/askpass_request.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <grp.h>
#include <iterator>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace {

  std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  std::optional<pid_t> parentPid(const std::filesystem::path& procRoot, pid_t pid) {
    const auto stat = readFile(procRoot / std::to_string(pid) / "stat");
    if (!stat) {
      return std::nullopt;
    }
    // "pid (comm) state ppid ...": comm may hold spaces and ')', so parse after the last ')'.
    const auto close = stat->rfind(')');
    if (close == std::string::npos) {
      return std::nullopt;
    }
    int ppid = 0;
    char state = 0;
    if (std::sscanf(stat->c_str() + close + 1, " %c %d", &state, &ppid) != 2 || ppid <= 0) {
      return std::nullopt;
    }
    return static_cast<pid_t>(ppid);
  }

  std::string quoteArg(std::string_view arg) {
    std::string out;
    bool escaped = false;
    const auto hidden = [](char32_t cp) {
      return (cp >= 0x80 && cp <= 0x9f) || cp == 0x61c || (cp >= 0x200b && cp <= 0x200f) ||
             (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x2069) || cp == 0xfeff;
    };
    for (std::size_t i = 0; i < arg.size();) {
      const auto byte = static_cast<unsigned char>(arg[i]);
      if (byte < 0x80) {
        if (byte < 0x20 || byte == 0x7f) {
          out += std::format("\\x{:02x}", byte);
          escaped = true;
        } else if (byte == '\'') {
          out += "'\\''";
        } else {
          out += static_cast<char>(byte);
        }
        ++i;
        continue;
      }
      const std::size_t len = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc2 ? 2 : 0;
      char32_t cp = len == 4 ? byte & 0x07 : len == 3 ? byte & 0x0f : byte & 0x1f;
      bool valid = len != 0 && byte <= 0xf4 && i + len <= arg.size();
      for (std::size_t k = 1; valid && k < len; ++k) {
        const auto cont = static_cast<unsigned char>(arg[i + k]);
        valid = (cont & 0xc0) == 0x80;
        cp = (cp << 6) | (cont & 0x3f);
      }
      if (valid) {
        valid = (len == 2 || (len == 3 ? cp >= 0x800 : cp >= 0x10000 && cp <= 0x10ffff)) && (cp < 0xd800 || cp > 0xdfff);
      }
      if (!valid) {
        out += std::format("\\x{:02x}", byte);
        escaped = true;
        ++i;
      } else if (hidden(cp)) {
        out += std::format("\\u{{{:04x}}}", static_cast<unsigned>(cp));
        escaped = true;
        i += len;
      } else {
        out.append(arg.substr(i, len));
        i += len;
      }
    }
    const bool quote = escaped || arg.empty() || arg.find_first_of(" \t\n'\"\\$`;&|<>(){}*?!#~") != std::string_view::npos;
    if (!quote) {
      return out;
    }
    return "'" + out + "'";
  }

  std::string truncateCodePoints(std::string text, std::size_t maxChars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
        continue;
      }
      if (chars == maxChars) {
        text.resize(i);
        text += "…";
        break;
      }
      ++chars;
    }
    return text;
  }

} // namespace

std::optional<AskpassRequest> readAskpassRequest(const std::filesystem::path& procRoot, pid_t askpassPid) {
  const auto parent = parentPid(procRoot, askpassPid);
  if (!parent) {
    return std::nullopt;
  }
  const auto cmdline = readFile(procRoot / std::to_string(*parent) / "cmdline");
  if (!cmdline || cmdline->empty()) {
    return std::nullopt;
  }
  std::string command;
  std::string_view rest = *cmdline;
  if (rest.ends_with('\0')) {
    rest.remove_suffix(1);
  }
  while (true) {
    const auto end = rest.find('\0');
    if (!command.empty()) {
      command += ' ';
    }
    command += quoteArg(rest.substr(0, end));
    if (end == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(end + 1);
  }

  AskpassRequest request{.command = truncateCodePoints(std::move(command), kAskpassCommandMaxChars)};
  if (const auto requester = parentPid(procRoot, *parent)) {
    const std::filesystem::path dir = procRoot / std::to_string(*requester);
    request.requesterPid = *requester;
    std::string comm = readFile(dir / "comm").value_or(std::string());
    while (comm.ends_with('\n')) {
      comm.pop_back();
    }
    request.requester = quoteArg(comm);
    std::error_code ec;
    const auto cwd = std::filesystem::read_symlink(dir / "cwd", ec);
    if (!ec) {
      request.cwd = cwd.string();
    }
  }
  return request;
}

namespace {

  // The value fields of a /proc/<pid>/status line such as "Uid:\t1000\t0\t0\t0".
  std::optional<std::vector<long>> statusField(const std::filesystem::path& procRoot, pid_t pid, std::string_view key) {
    const auto status = readFile(procRoot / std::to_string(pid) / "status");
    if (!status) {
      return std::nullopt;
    }
    std::istringstream lines(*status);
    for (std::string line; std::getline(lines, line);) {
      if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ':') {
        continue;
      }
      std::istringstream values(line.substr(key.size() + 1));
      std::vector<long> out;
      for (long value = 0; values >> value;) {
        out.push_back(value);
      }
      return out;
    }
    return std::nullopt;
  }

} // namespace

std::optional<gid_t> askpassGroup() {
  static const std::optional<gid_t> gid = []() -> std::optional<gid_t> {
    const group* entry = ::getgrnam("andromeda-askpass");
    return entry != nullptr ? std::optional<gid_t>(entry->gr_gid) : std::nullopt;
  }();
  return gid;
}

std::expected<AskpassCaller, std::string> verifyAskpassCaller(const AskpassProc& proc, pid_t peerPid, gid_t peerGid) {
  // Replaces an exe check: the shell cannot read /proc/<pid>/exe of a setgid process.
  if (!proc.helperGid || peerGid != *proc.helperGid) {
    return std::unexpected(std::string("the caller is not andromeda-askpass"));
  }
  const auto parent = parentPid(proc.procRoot, peerPid);
  if (!parent) {
    return std::unexpected(std::string("the caller has no parent"));
  }
  // Residual risk: sudo is known by effective uid 0 and comm, not by exe: a user cannot read a setuid process's exe.
  const auto uids = statusField(proc.procRoot, *parent, "Uid");
  std::string comm = readFile(proc.procRoot / std::to_string(*parent) / "comm").value_or(std::string());
  while (comm.ends_with('\n')) {
    comm.pop_back();
  }
  if (!uids || uids->size() < 2 || (*uids)[0] != static_cast<long>(proc.uid) || (*uids)[1] != 0 || comm != "sudo") {
    return std::unexpected(std::string("the caller was not started by sudo"));
  }
  auto request = readAskpassRequest(proc.procRoot, peerPid);
  if (!request) {
    return std::unexpected(std::string("cannot read the sudo command"));
  }
  return AskpassCaller{.parentPid = *parent, .request = std::move(*request)};
}

std::optional<std::string> recheckAskpassCaller(const AskpassProc& proc, pid_t peerPid, pid_t expectedParent) {
  struct stat file{};
  if (::stat((proc.procRoot / std::to_string(peerPid) / "stat").c_str(), &file) != 0
      || file.st_uid != proc.nonDumpableOwner) {
    return std::string("the caller is dumpable");
  }
  const auto tracer = statusField(proc.procRoot, peerPid, "TracerPid");
  if (!tracer || tracer->size() != 1 || (*tracer)[0] != 0) {
    return std::string("the caller is traced");
  }
  // A dead parent reparents the peer, so an unchanged ppid also pins the parent's pidfd to the right process.
  if (parentPid(proc.procRoot, peerPid) != std::optional<pid_t>(expectedParent)) {
    return std::string("the caller's parent changed");
  }
  return std::nullopt;
}
