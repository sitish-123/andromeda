#include "auth/password_changer.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <pty.h>
#include <sdbus-c++/sdbus-c++.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

extern char** environ;

namespace password_change {
  namespace {

    constexpr Logger kLog("password");
    // passwd waits on PAM; pam_faillock adds a delay after a wrong password.
    constexpr int kPromptTimeoutMs = 15000;

    void clear(std::string& s) {
      volatile char* p = s.data();
      for (std::size_t i = 0; i < s.size(); ++i) {
        p[i] = '\0';
      }
      s.clear();
    }

    void clear(std::vector<std::uint8_t>& v) {
      volatile std::uint8_t* p = v.data();
      for (std::size_t i = 0; i < v.size(); ++i) {
        p[i] = 0;
      }
      v.clear();
    }

    std::string lower(std::string s) {
      std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    }

    // The pty output passwd wrote, lowercased, read until one of `needles` shows after `from`, EOF or the timeout.
    // Returns the index of the needle that matched, or -1.
    int waitFor(int fd, std::string& output, std::size_t& from, std::initializer_list<std::string_view> needles) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPromptTimeoutMs);
      while (true) {
        const std::string low = lower(output.substr(from));
        int index = 0;
        for (std::string_view needle : needles) {
          if (const auto at = low.find(needle); at != std::string::npos) {
            from += at + needle.size();
            return index;
          }
          ++index;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
          return -1;
        }
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready <= 0) {
          return -1;
        }
        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          return -1; // EIO: passwd closed the pty.
        }
        output.append(buf, static_cast<std::size_t>(n));
      }
    }

    // passwd prints the prompt before it turns echo off; a write in between would echo the secret into the output.
    // The master side reports the slave's termios on Linux.
    bool waitEchoOff(int fd) {
      for (int i = 0; i < 200; ++i) {
        termios t{};
        if (::tcgetattr(fd, &t) == 0 && (t.c_lflag & ECHO) == 0) {
          return true;
        }
        ::usleep(5000);
      }
      return false;
    }

    bool writeLine(int fd, const std::string& text) {
      if (!waitEchoOff(fd)) {
        return false;
      }
      std::string line = text + "\n";
      const char* p = line.data();
      std::size_t left = line.size();
      while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          clear(line);
          return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
      }
      clear(line);
      return true;
    }

    // The last non-empty line passwd printed: its reason for a refusal.
    std::string lastLine(const std::string& output) {
      std::string text = output;
      std::erase(text, '\r');
      std::string last;
      std::size_t start = 0;
      while (start <= text.size()) {
        const auto end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        // Skip the prompts ("New password:"), keep reasons such as "BAD PASSWORD: it is too short".
        const auto textEnd = line.find_last_not_of(" \t");
        if (textEnd != std::string::npos && !lower(line.substr(0, textEnd + 1)).ends_with("password:")) {
          last = line;
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return last;
    }

    Result runPasswd(const std::string& current, const std::string& next) {
      // The child may only make async-signal-safe calls (the shell has threads): build its environment first.
      std::vector<std::string> env;
      for (char** e = environ; *e != nullptr; ++e) {
        if (!std::string_view(*e).starts_with("LC_ALL=")) {
          env.emplace_back(*e);
        }
      }
      env.emplace_back("LC_ALL=C");
      std::vector<char*> envp;
      for (auto& entry : env) {
        envp.push_back(entry.data());
      }
      envp.push_back(nullptr);
      char arg0[] = "passwd";
      char* argv[] = {arg0, nullptr};
      int master = -1;
      const pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
      if (pid < 0) {
        return {.message = std::strerror(errno)};
      }
      if (pid == 0) {
        ::execve("/usr/bin/passwd", argv, envp.data());
        ::execve("/bin/passwd", argv, envp.data());
        ::_exit(127);
      }

      std::string output;
      std::size_t from = 0;
      Result result;
      const auto finish = [&](bool kill) {
        if (kill) {
          ::kill(pid, SIGTERM);
        }
        ::close(master);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      };

      // Root and some PAM stacks skip the current-password prompt.
      const int first = waitFor(master, output, from, {"current password:", "(current) unix password:", "new password:"});
      if (first < 0) {
        result.message = lastLine(output);
        finish(true);
        return result;
      }
      if (first != 2) {
        if (!writeLine(master, current)) {
          finish(true);
          return result;
        }
        const int asked = waitFor(master, output, from, {"new password:", "authentication", "failure"});
        if (asked != 0) {
          result.wrongCurrent = true;
          result.message = lastLine(output);
          finish(asked < 0);
          return result;
        }
      }
      if (!writeLine(master, next)) {
        finish(true);
        return result;
      }
      // A refused new password prints the reason, then asks for a new one again.
      const int retype = waitFor(master, output, from, {"retype new password:", "new password:"});
      if (retype != 0) {
        result.message = lastLine(output);
        finish(true);
        return result;
      }
      if (!writeLine(master, next)) {
        finish(true);
        return result;
      }
      const int after = waitFor(master, output, from, {"new password:"});
      if (after == 0) {
        result.message = lastLine(output);
        finish(true);
        return result;
      }
      result.ok = finish(false);
      if (!result.ok) {
        result.message = lastLine(output);
      }
      return result;
    }

    // True when a `password` line of /etc/pam.d/<service>, or of a stack it includes, runs pam_gnome_keyring.
    bool pamChangesKeyring(const std::string& service, int depth) {
      if (depth > 4 || service.find('/') != std::string::npos) {
        return false;
      }
      std::ifstream file("/etc/pam.d/" + service);
      for (std::string line; std::getline(file, line);) {
        std::istringstream words(line);
        std::string type;
        std::string control;
        std::string module;
        words >> type >> control >> module;
        if (type.starts_with("-")) {
          type.erase(0, 1);
        }
        if (type != "password") {
          continue;
        }
        if (control == "include" || control == "substack") {
          if (pamChangesKeyring(module, depth + 1)) {
            return true;
          }
        } else if (module.find("pam_gnome_keyring") != std::string::npos) {
          return true;
        }
      }
      return false;
    }

    using Secret = sdbus::Struct<sdbus::ObjectPath, std::vector<std::uint8_t>, std::vector<std::uint8_t>, std::string>;

    // gnome-keyring keeps the login keyring under the old password unless pam_gnome_keyring is in the password
    // stack; this sets it the way gnome-control-center does. A missing daemon or login keyring is not a failure.
    bool changeKeyring(const std::string& current, const std::string& next) {
      try {
        auto bus = sdbus::createSessionBusConnection();
        const sdbus::ServiceName service{"org.freedesktop.secrets"};
        auto secrets = sdbus::createProxy(*bus, service, sdbus::ObjectPath{"/org/freedesktop/secrets"});
        sdbus::ObjectPath collection;
        secrets->callMethod("ReadAlias").onInterface("org.freedesktop.Secret.Service").withArguments(std::string{"login"})
            .storeResultsTo(collection);
        if (collection == "/") {
          return true;
        }
        sdbus::Variant output;
        sdbus::ObjectPath session;
        secrets->callMethod("OpenSession")
            .onInterface("org.freedesktop.Secret.Service")
            .withArguments(std::string{"plain"}, sdbus::Variant{std::string{}})
            .storeResultsTo(output, session);
        std::vector<std::uint8_t> oldBytes(current.begin(), current.end());
        std::vector<std::uint8_t> newBytes(next.begin(), next.end());
        Secret original{session, std::vector<std::uint8_t>{}, oldBytes, std::string{"text/plain"}};
        Secret master{session, std::vector<std::uint8_t>{}, newBytes, std::string{"text/plain"}};
        bool ok = true;
        try {
          secrets->callMethod("ChangeWithMasterPassword")
              .onInterface("org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface")
              .withArguments(collection, original, master);
        } catch (const sdbus::Error& e) {
          // Another Secret Service (KeePassXC, KWallet) has no such call: its keyring is not tied to the login.
          const std::string_view name = e.getName();
          ok = name == "org.freedesktop.DBus.Error.UnknownMethod" || name == "org.freedesktop.DBus.Error.UnknownInterface";
          if (!ok) {
            kLog.warn("login keyring password not changed: {}", e.what());
          }
        }
        clear(oldBytes);
        clear(newBytes);
        clear(std::get<2>(original));
        clear(std::get<2>(master));
        auto sessionProxy = sdbus::createProxy(*bus, service, session);
        sessionProxy->callMethod("Close").onInterface("org.freedesktop.Secret.Session");
        return ok;
      } catch (const sdbus::Error& e) {
        if (std::string_view(e.getName()) == "org.freedesktop.DBus.Error.ServiceUnknown") {
          return true;
        }
        kLog.warn("login keyring not reachable: {}", e.what());
        return false;
      }
    }

  } // namespace

  Result change(std::string& current, std::string& next) {
    Result result = runPasswd(current, next);
    if (result.ok) {
      // With pam_gnome_keyring in the passwd stack, PAM has already changed the keyring; a second change would fail.
      result.keyringFailed = !pamChangesKeyring("passwd", 0) && !changeKeyring(current, next);
      kLog.info("password changed (keyring {})", result.keyringFailed ? "kept the old password" : "follows");
    } else {
      kLog.info("password not changed: {}", result.wrongCurrent ? "wrong current password" : result.message);
    }
    clear(current);
    clear(next);
    return result;
  }

} // namespace password_change
