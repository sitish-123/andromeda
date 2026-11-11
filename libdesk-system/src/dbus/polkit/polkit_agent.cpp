#include "dbus/polkit/polkit_agent.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IObject.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <sdbus-c++/VTableItems.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-login.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

  constexpr Logger kLog("polkit");
  constexpr auto kAgentObjectPath = "/org/noctalia/PolkitAuthenticationAgent";
  constexpr auto kAgentInterface = "org.freedesktop.PolicyKit1.AuthenticationAgent";
  constexpr auto kAuthorityInterface = "org.freedesktop.PolicyKit1.Authority";
  const sdbus::ServiceName kAuthorityName{"org.freedesktop.PolicyKit1"};
  const sdbus::ObjectPath kAuthorityPath{"/org/freedesktop/PolicyKit1/Authority"};
  constexpr auto kErrorCancelled = "org.freedesktop.PolicyKit1.Error.Cancelled";
  constexpr auto kErrorFailed = "org.freedesktop.PolicyKit1.Error.Failed";

  // polkit >= 126 runs the helper per connection on this socket; older or setuid setups spawn it.
  constexpr auto kHelperSocket = "/run/polkit/agent-helper.socket";
  constexpr std::array kHelperPaths{"/usr/lib/polkit-1/polkit-agent-helper-1", "/usr/libexec/polkit-agent-helper-1"};
  constexpr std::size_t kMaxHelperOutput = 64 * 1024;

  using Subject = sdbus::Struct<std::string, std::map<std::string, sdbus::Variant>>;

  std::optional<std::string> usernameFromUid(uid_t uid) {
    passwd pwd{};
    passwd* result = nullptr;
    std::array<char, 4096> buffer{};
    const int rc = getpwuid_r(uid, &pwd, buffer.data(), buffer.size(), &result);
    if (rc != 0 || result == nullptr || result->pw_name == nullptr || result->pw_name[0] == '\0') {
      return std::nullopt;
    }
    return std::string(result->pw_name);
  }

  // The GECOS full name (the field before the first comma); empty when unset.
  std::string realNameFromUid(uid_t uid) {
    passwd pwd{};
    passwd* result = nullptr;
    std::array<char, 4096> buffer{};
    if (getpwuid_r(uid, &pwd, buffer.data(), buffer.size(), &result) != 0 || result == nullptr
        || result->pw_gecos == nullptr) {
      return {};
    }
    const std::string_view gecos = result->pw_gecos;
    return std::string(gecos.substr(0, gecos.find(',')));
  }

  std::optional<std::uint32_t> variantId(const sdbus::Variant& value) {
    if (value.containsValueOfType<std::uint32_t>()) {
      return value.get<std::uint32_t>();
    }
    if (value.containsValueOfType<std::int32_t>()) {
      return static_cast<std::uint32_t>(value.get<std::int32_t>());
    }
    return std::nullopt;
  }

  std::vector<PolkitRequestIdentity> parseIdentities(const std::vector<Subject>& identities) {
    std::vector<PolkitRequestIdentity> out;
    for (const Subject& identity : identities) {
      const std::string& kind = std::get<0>(identity);
      const auto& details = std::get<1>(identity);
      const char* key = kind == "unix-user" ? "uid" : kind == "unix-group" ? "gid" : nullptr;
      const auto it = key != nullptr ? details.find(key) : details.end();
      if (it == details.end()) {
        continue;
      }
      const auto id = variantId(it->second);
      if (!id.has_value()) {
        continue;
      }
      PolkitRequestIdentity entry{.kind = kind, .uid = *id};
      if (kind == "unix-user") {
        entry.userName = usernameFromUid(static_cast<uid_t>(*id)).value_or(std::to_string(*id));
        entry.realName = realNameFromUid(static_cast<uid_t>(*id));
      }
      const bool duplicate = std::ranges::any_of(out, [&entry](const PolkitRequestIdentity& existing) {
        return existing.kind == entry.kind && existing.uid == entry.uid;
      });
      if (!duplicate) {
        out.push_back(std::move(entry));
      }
    }
    return out;
  }

  // The helper writes PAM text through g_strescape; this reverses it.
  std::string unescapeHelperText(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
      if (in[i] != '\\' || i + 1 == in.size()) {
        out += in[i];
        continue;
      }
      const char c = in[++i];
      if (c >= '0' && c <= '7') {
        int value = 0;
        std::size_t digits = 0;
        while (digits < 3 && i < in.size() && in[i] >= '0' && in[i] <= '7') {
          value = value * 8 + (in[i] - '0');
          ++i;
          ++digits;
        }
        --i;
        out += static_cast<char>(value);
        continue;
      }
      switch (c) {
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'v': out += '\v'; break;
      default: out += c; break;
      }
    }
    return out;
  }

  std::optional<std::string_view> afterPrefix(std::string_view line, std::string_view prefix) {
    if (!line.starts_with(prefix)) {
      return std::nullopt;
    }
    line.remove_prefix(prefix.size());
    if (line.empty()) {
      return line;
    }
    if (line.front() != ' ') {
      return std::nullopt;
    }
    return line.substr(1);
  }

  std::optional<std::uint64_t> processStartTime(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(file, stat)) {
      return std::nullopt;
    }
    const auto commEnd = stat.rfind(')');
    if (commEnd == std::string::npos) {
      return std::nullopt;
    }
    std::istringstream fields(stat.substr(commEnd + 1));
    std::string field;
    // Field 22 of proc_pid_stat(5); the stream starts at field 3.
    for (int index = 3; index <= 22; ++index) {
      if (!(fields >> field)) {
        return std::nullopt;
      }
    }
    return std::strtoull(field.c_str(), nullptr, 10);
  }

  std::string agentLocale() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
      if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
        return value;
      }
    }
    return "C";
  }

  std::optional<Subject> agentSubject(std::string& error) {
    const char* testPid = std::getenv("ANDROMEDA_POLKIT_TEST_PROCESS");
    if (testPid == nullptr) {
      testPid = std::getenv("NOCTALIA_POLKIT_TEST_PROCESS");
    }
    // E2E only: serve one process, so a test pkexec reaches this agent while the user's own agent keeps the session.
    if (testPid != nullptr && testPid[0] != '\0') {
      const auto pid = static_cast<pid_t>(std::strtol(testPid, nullptr, 10));
      const auto startTime = processStartTime(pid);
      struct stat owner{};
      if (!startTime.has_value() || ::stat(("/proc/" + std::to_string(pid)).c_str(), &owner) != 0) {
        error = "test process " + std::to_string(pid) + " is not running";
        return std::nullopt;
      }
      return Subject{
          "unix-process", std::map<std::string, sdbus::Variant>{
                              {"pid", sdbus::Variant{static_cast<std::uint32_t>(pid)}},
                              {"start-time", sdbus::Variant{*startTime}},
                              {"uid", sdbus::Variant{static_cast<std::int32_t>(owner.st_uid)}},
                          }
      };
    }
    std::string sessionId;
    if (const char* env = std::getenv("XDG_SESSION_ID"); env != nullptr && env[0] != '\0') {
      sessionId = env;
    } else if (char* session = nullptr; sd_pid_get_session(0, &session) >= 0 && session != nullptr) {
      sessionId = session;
      std::free(session);
    }
    if (!sessionId.empty()) {
      return Subject{
          "unix-session", std::map<std::string, sdbus::Variant>{{"session-id", sdbus::Variant{sessionId}}}
      };
    }
    kLog.info("polkit: no logind session for pid; trying unix-user authentication agent");
    return Subject{
        "unix-user", std::map<std::string, sdbus::Variant>{{"uid", sdbus::Variant{static_cast<std::uint32_t>(::getuid())}}}
    };
  }

  // One run of polkit-agent-helper-1: it runs PAM for the cookie and reports the result to polkitd itself. Line
  // protocol: we send the user and cookie, it sends PAM_PROMPT_ECHO_OFF/ON, PAM_ERROR_MSG, PAM_TEXT_INFO lines and
  // ends with SUCCESS or FAILURE; each answer goes back as one line.
  class HelperConversation {
  public:
    HelperConversation(int fd, pid_t pid) : m_fd(fd), m_pid(pid) {}

    ~HelperConversation() {
      ::close(m_fd);
      if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
        ::waitpid(m_pid, nullptr, 0);
      }
    }

    HelperConversation(const HelperConversation&) = delete;
    HelperConversation& operator=(const HelperConversation&) = delete;

    static std::unique_ptr<HelperConversation> open(const std::string& user, const std::string& cookie) {
      if (::access(kHelperSocket, F_OK) == 0) {
        if (auto conversation = connectSocket(); conversation != nullptr) {
          return conversation->send(user + '\n' + cookie + '\n') ? std::move(conversation) : nullptr;
        }
      }
      auto conversation = spawn(user);
      if (conversation == nullptr) {
        return nullptr;
      }
      return conversation->send(cookie + '\n') ? std::move(conversation) : nullptr;
    }

    [[nodiscard]] int fd() const noexcept { return m_fd; }

    bool send(std::string_view data) {
      while (!data.empty()) {
        const ssize_t n = ::send(m_fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
      }
      return true;
    }

    // Appends the complete lines that are ready; false once the helper has closed its end.
    bool readLines(std::vector<std::string>& lines) {
      std::array<char, 1024> chunk{};
      bool open = true;
      while (true) {
        const ssize_t n = ::recv(m_fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
        if (n > 0) {
          m_buffer.append(chunk.data(), static_cast<std::size_t>(n));
          if (m_buffer.size() > kMaxHelperOutput) {
            open = false;
            break;
          }
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          break;
        }
        open = false;
        break;
      }
      std::size_t start = 0;
      for (std::size_t end = m_buffer.find('\n'); end != std::string::npos; end = m_buffer.find('\n', start)) {
        lines.emplace_back(m_buffer.substr(start, end - start));
        start = end + 1;
      }
      m_buffer.erase(0, start);
      return open;
    }

  private:
    static std::unique_ptr<HelperConversation> connectSocket() {
      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return nullptr;
      }
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::strncpy(addr.sun_path, kHelperSocket, sizeof(addr.sun_path) - 1);
      if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        kLog.warn("polkit helper socket {}: {}; spawning the helper", kHelperSocket, std::strerror(errno));
        ::close(fd);
        return nullptr;
      }
      return std::make_unique<HelperConversation>(fd, -1);
    }

    static std::unique_ptr<HelperConversation> spawn(const std::string& user) {
      const char* helper = nullptr;
      for (const char* path : kHelperPaths) {
        if (::access(path, X_OK) == 0) {
          helper = path;
          break;
        }
      }
      if (helper == nullptr) {
        kLog.warn("polkit-agent-helper-1 not found");
        return nullptr;
      }
      std::array<int, 2> sockets{};
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0) {
        return nullptr;
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
      std::array<char*, 3> argv{const_cast<char*>(helper), const_cast<char*>(user.c_str()), nullptr};
      pid_t pid = -1;
      const int rc = ::posix_spawn(&pid, helper, &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      ::close(sockets[1]);
      if (rc != 0) {
        kLog.warn("could not start {}: {}", helper, std::strerror(rc));
        ::close(sockets[0]);
        return nullptr;
      }
      return std::make_unique<HelperConversation>(sockets[0], pid);
    }

    int m_fd;
    pid_t m_pid;
    std::string m_buffer;
  };

  // pam_gaze sends these instead of English text to services on gazed's PamInternal list.
  constexpr std::string_view kGazeConfirmRequest = "GAZE_REQUIRE_CONFIRMATION";
  constexpr std::string_view kGazeConfirmed = "GAZE_CONFIRMED";
  // gaze_core::dbus::GAZE_FACE_ON_REQUEST_PROMPT (pam_gaze `on-request`); matched exactly.
  constexpr std::string_view kGazeFaceOnRequestPrompt = "Password (Enter for face): ";

  struct GazeMarker {
    PolkitFaceState state = PolkitFaceState::None;
    std::string text;
  };

  // nullopt when the text is not a marker; an unknown marker maps to no text so it is never shown raw.
  std::optional<GazeMarker> parseGazeMarker(std::string_view text) {
    if (!text.starts_with("GAZE_")) {
      return std::nullopt;
    }
    kLog.info("face prompt {}", text);
    using S = PolkitFaceState;
    if (text == "GAZE_MSG_LOOK_CAMERA" || text == "GAZE_MSG_LOOK_OR_PASSWORD") {
      return GazeMarker{S::Looking, i18n::tr("auth.face.polkit-look")};
    }
    if (text == "GAZE_MSG_FACE_VERIFIED") {
      return GazeMarker{S::Verified, i18n::tr("auth.face.verified")};
    }
    if (text == kGazeConfirmRequest) {
      return GazeMarker{S::Verified, i18n::tr("auth.face.verified-confirm")};
    }
    if (text == "GAZE_MSG_FACE_NOT_RECOGNIZED") {
      return GazeMarker{S::Failed, i18n::tr("auth.face.no-match")};
    }
    if (text == "GAZE_MSG_FACE_NOT_DETECTED") {
      return GazeMarker{S::Failed, i18n::tr("auth.face.no-face")};
    }
    if (text == "GAZE_MSG_FACE_TOO_DARK") {
      return GazeMarker{S::Failed, i18n::tr("auth.face.too-dark")};
    }
    if (text == "GAZE_MSG_FACE_TIMED_OUT") {
      return GazeMarker{S::Failed, i18n::tr("auth.face.timed-out")};
    }
    if (text == "GAZE_MSG_FACE_UNAVAILABLE") {
      return GazeMarker{S::Failed, i18n::tr("auth.face.unavailable")};
    }
    kLog.debug("ignoring unknown gaze marker {}", text);
    return GazeMarker{};
  }

  struct PendingAuth {
    std::string actionId;
    std::string message;
    std::string iconName;
    std::string cookie;
    bool isInternal = false;
    std::vector<PolkitRequestIdentity> identities;
    // Set by the first conversation (or the user's choice) and kept for retries.
    std::optional<std::uint32_t> activeUid;
    sdbus::Result<> result;
  };

  enum class Registration { Idle, Registering, Registered };

} // namespace

struct PolkitAgent::Impl {
  SystemBus& bus;
  StateCallback stateCallback;
  ReadyCallback readyCallback;
  std::unique_ptr<sdbus::IObject> object;
  std::unique_ptr<sdbus::IProxy> authority;
  Subject subject;
  Registration registration = Registration::Idle;
  bool m_nextInternal = false;
  std::unique_ptr<PendingAuth> pending;
  std::unique_ptr<HelperConversation> conversation;
  // Bumped per conversation, so a line handler can tell that the one it reads from has ended.
  std::uint64_t conversationSerial = 0;
  // A FAILURE before any answer would restart the helper in a loop; only an answered attempt retries.
  bool answered = false;
  std::string activeUser;

  bool responseRequired = false;
  bool responseVisible = false;
  std::string inputPrompt;
  std::string supplementaryMessage;
  bool supplementaryError = false;
  PolkitFaceState face = PolkitFaceState::None;
  bool faceConfirm = false;
  bool faceConfirmIsPrompt = false;
  bool faceOnRequest = false;

  mutable int polledFd = -1;

  explicit Impl(SystemBus& systemBus) : bus(systemBus) {}

  ~Impl() {
    finishPending(kErrorCancelled, "PolkitAgent is being destroyed");
    if (registration != Registration::Idle && authority != nullptr) {
      try {
        authority->callMethod("UnregisterAuthenticationAgent")
            .onInterface(kAuthorityInterface)
            .withArguments(subject, std::string{kAgentObjectPath})
            .dontExpectReply();
      } catch (const sdbus::Error& e) {
        kLog.debug("UnregisterAuthenticationAgent failed: {}", e.what());
      }
    }
  }

  void ready(bool ok, const std::string& error) {
    if (readyCallback) {
      readyCallback(ok, error);
    }
  }

  void start() {
    if (registration != Registration::Idle) {
      return;
    }
    std::string error;
    auto agentSubjectValue = agentSubject(error);
    if (!agentSubjectValue.has_value()) {
      ready(false, error);
      return;
    }
    subject = std::move(*agentSubjectValue);
    try {
      object = sdbus::createObject(bus.connection(), sdbus::ObjectPath{kAgentObjectPath});
      object
          ->addVTable(
              sdbus::registerMethod("BeginAuthentication")
                  .withInputParamNames("action_id", "message", "icon_name", "details", "cookie", "identities")
                  .implementedAs([this](
                                     sdbus::Result<>&& result, const std::string& actionId, const std::string& message,
                                     const std::string& iconName, const std::map<std::string, std::string>& /*details*/,
                                     const std::string& cookie, const std::vector<Subject>& identities
                                 ) {
                    guard("BeginAuthentication", [&] {
                      beginAuthentication(std::move(result), actionId, message, iconName, cookie, identities);
                    });
                  }),
              sdbus::registerMethod("CancelAuthentication")
                  .withInputParamNames("cookie")
                  .implementedAs([this](const std::string& cookie) {
                    guard("CancelAuthentication", [&] { cancelFromAuthority(cookie); });
                  })
          )
          .forInterface(kAgentInterface);
      authority = sdbus::createProxy(bus.connection(), kAuthorityName, kAuthorityPath);
      registration = Registration::Registering;
      authority->callMethodAsync("RegisterAuthenticationAgent")
          .onInterface(kAuthorityInterface)
          .withArguments(subject, agentLocale(), std::string{kAgentObjectPath})
          .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
            if (e.has_value()) {
              registration = Registration::Idle;
              ready(false, e->getMessage());
              return;
            }
            registration = Registration::Registered;
            kLog.info("registered Polkit authentication agent at {}", kAgentObjectPath);
            ready(true, std::string{});
          });
    } catch (const sdbus::Error& e) {
      registration = Registration::Idle;
      ready(false, e.getMessage());
    }
  }

  // sd-bus dispatches these handlers from C; nothing may unwind through it.
  template <typename F> static void guard(const char* name, F&& body) noexcept {
    try {
      std::forward<F>(body)();
    } catch (const std::exception& e) {
      kLog.warn("exception in polkit {} ({}); ignoring", name, e.what());
    } catch (...) {
      kLog.warn("unknown exception in polkit {}; ignoring", name);
    }
  }

  [[nodiscard]] bool fromAuthority() const {
    const char* sender = object->getCurrentlyProcessedMessage().getSender();
    return sender != nullptr && bus.isNameOwner(kAuthorityName, sender);
  }

  void emitStateChanged() {
    if (stateCallback) {
      stateCallback();
    }
  }

  void clearConversationState() {
    face = PolkitFaceState::None;
    faceConfirm = false;
    faceConfirmIsPrompt = false;
    faceOnRequest = false;
    responseRequired = false;
    responseVisible = false;
    inputPrompt.clear();
    supplementaryMessage.clear();
    supplementaryError = false;
  }

  void stopConversation() {
    conversation.reset();
    ++conversationSerial;
    answered = false;
    activeUser.clear();
  }

  // errorName null: the user authenticated. BeginAuthentication replies only here, when the attempt ends.
  void finishPending(const char* errorName, const std::string& reason) {
    stopConversation();
    if (pending != nullptr) {
      try {
        if (errorName == nullptr) {
          pending->result.returnResults();
        } else {
          pending->result.returnError(sdbus::Error{sdbus::Error::Name{errorName}, reason});
        }
      } catch (const sdbus::Error& e) {
        kLog.warn("BeginAuthentication reply failed: {}", e.what());
      }
    }
    pending.reset();
    clearConversationState();
  }

  void clearPending(const char* errorName, const std::string& reason) {
    finishPending(errorName, reason);
    emitStateChanged();
  }

  void beginAuthentication(
      sdbus::Result<>&& result, const std::string& actionId, const std::string& message, const std::string& iconName,
      const std::string& cookie, const std::vector<Subject>& identities
  ) {
    if (!fromAuthority()) {
      kLog.warn("BeginAuthentication from a peer that is not polkitd; refused");
      result.returnError(sdbus::Error{sdbus::Error::Name{kErrorFailed}, "only polkitd may begin authentication"});
      return;
    }
    if (pending != nullptr) {
      finishPending(kErrorCancelled, "Replaced by a newer authentication request");
    }
    auto request = std::make_unique<PendingAuth>(PendingAuth{
        .actionId = actionId,
        .message = message,
        .iconName = iconName,
        .cookie = cookie,
        .isInternal = m_nextInternal,
        .identities = parseIdentities(identities),
        .result = std::move(result),
    });
    m_nextInternal = false;
    if (request->identities.empty()) {
      kLog.warn("polkit request \"{}\" has no identities", request->actionId);
      request->result.returnError(sdbus::Error{sdbus::Error::Name{kErrorFailed}, "no identities to authenticate"});
      return;
    }
    pending = std::move(request);
    kLog.info("authentication request for \"{}\"", pending->actionId);
    clearConversationState();
    if (!startConversation()) {
      kLog.warn("polkit session startup failed for action \"{}\"", pending->actionId);
      clearPending(kErrorFailed, "Failed to start authentication session");
      return;
    }
    emitStateChanged();
  }

  bool startConversation() {
    stopConversation();
    if (pending == nullptr) {
      return false;
    }
    // Prefer the current user when several wheel members are offered.
    const auto selfUid = static_cast<std::uint32_t>(::geteuid());
    const PolkitRequestIdentity* chosen = nullptr;
    const std::uint32_t wanted = pending->activeUid.value_or(selfUid);
    for (const PolkitRequestIdentity& identity : pending->identities) {
      if (identity.kind == "unix-user" && (chosen == nullptr || identity.uid == wanted)) {
        chosen = &identity;
        if (identity.uid == wanted) {
          break;
        }
      }
    }
    if (chosen == nullptr) {
      kLog.warn("polkit action \"{}\" has no unix-user identity (unix-group alone is unsupported)", pending->actionId);
      return false;
    }
    const auto user = usernameFromUid(static_cast<uid_t>(chosen->uid));
    if (!user.has_value()) {
      kLog.warn("polkit identity uid {} has no user name", chosen->uid);
      return false;
    }
    conversation = HelperConversation::open(*user, pending->cookie);
    if (conversation == nullptr) {
      return false;
    }
    activeUser = *user;
    pending->activeUid = chosen->uid;
    return true;
  }

  void selectIdentity(std::uint32_t uid) {
    if (pending == nullptr || pending->activeUid == uid
        || std::ranges::none_of(pending->identities, [uid](const PolkitRequestIdentity& identity) {
             return identity.kind == "unix-user" && identity.uid == uid;
           })) {
      return;
    }
    pending->activeUid = uid;
    clearConversationState();
    if (!startConversation()) {
      kLog.warn("polkit session start for uid {} failed for action \"{}\"", uid, pending->actionId);
      clearPending(kErrorFailed, "Failed to start authentication session");
      return;
    }
    emitStateChanged();
  }

  void handleHelperLine(std::string_view line) {
    if (line == "SUCCESS") {
      handleCompleted(true);
    } else if (line == "FAILURE") {
      handleCompleted(false);
    } else if (const auto off = afterPrefix(line, "PAM_PROMPT_ECHO_OFF")) {
      handleRequest(unescapeHelperText(*off), false);
    } else if (const auto on = afterPrefix(line, "PAM_PROMPT_ECHO_ON")) {
      handleRequest(unescapeHelperText(*on), true);
    } else if (const auto error = afterPrefix(line, "PAM_ERROR_MSG")) {
      setSupplementary(unescapeHelperText(*error), true);
    } else if (const auto info = afterPrefix(line, "PAM_TEXT_INFO")) {
      setSupplementary(unescapeHelperText(*info), false);
    } else {
      kLog.debug("ignoring polkit helper line of {} bytes", line.size());
    }
  }

  void handleRequest(const std::string& prompt, bool echoOn) {
    if (prompt == kGazeConfirmRequest) {
      kLog.info("face prompt {}", prompt);
      face = PolkitFaceState::Verified;
      faceConfirm = true;
      faceConfirmIsPrompt = true;
      inputPrompt = i18n::tr("auth.face.verified-confirm");
      responseVisible = false;
      responseRequired = true;
      emitStateChanged();
      return;
    }
    faceConfirmIsPrompt = false;
    faceOnRequest = prompt == kGazeFaceOnRequestPrompt;
    if (faceOnRequest) {
      inputPrompt = i18n::tr("auth.polkit.password-or-face");
      responseVisible = false;
      responseRequired = true;
      emitStateChanged();
      return;
    }
    inputPrompt = prompt.empty() ? i18n::tr("auth.polkit.default-message") : prompt;
    responseVisible = echoOn;
    responseRequired = true;
    emitStateChanged();
  }

  // gazed's verdict for the face check beside the open password prompt: pam_gaze cannot send a second message there.
  void faceCheckEnded(std::string text) {
    if (face != PolkitFaceState::Looking) {
      return;
    }
    face = PolkitFaceState::Failed;
    supplementaryMessage = std::move(text);
    supplementaryError = false;
    emitStateChanged();
  }

  void setSupplementary(const std::string& text, bool isError) {
    if (const auto marker = parseGazeMarker(text); marker.has_value()) {
      if (marker->text.empty()) {
        return;
      }
      face = marker->state;
      // Sent as info while the password prompt is still open: the next answer decides.
      faceConfirm = text == kGazeConfirmRequest;
      supplementaryMessage = marker->text;
      supplementaryError = false;
      emitStateChanged();
      return;
    }
    supplementaryMessage = text;
    supplementaryError = isError;
    emitStateChanged();
  }

  void sendAnswer(std::string_view answer) {
    std::string line;
    line.reserve(answer.size() + 1);
    line.append(answer);
    line.push_back('\n');
    // A failed send shows up as the helper closing its end, which dispatch() handles.
    conversation->send(line);
    explicit_bzero(line.data(), line.size());
    answered = true;
  }

  void confirmFace() {
    if (pending == nullptr || conversation == nullptr || !responseRequired || !faceConfirm) {
      return;
    }
    faceConfirm = false;
    faceConfirmIsPrompt = false;
    sendAnswer(kGazeConfirmed);
    responseRequired = false;
    inputPrompt.clear();
    supplementaryMessage = i18n::tr("auth.polkit.authenticating");
    supplementaryError = false;
    emitStateChanged();
  }

  void handleCompleted(bool gainedAuthorization) {
    if (pending == nullptr) {
      return;
    }
    if (gainedAuthorization) {
      kLog.info("polkit action \"{}\" authorized as {}", pending->actionId, activeUser);
      clearPending(nullptr, {});
      return;
    }
    if (!answered) {
      kLog.warn("polkit helper failed before any answer for \"{}\"", pending->actionId);
      clearPending(kErrorFailed, "Authentication failed");
      return;
    }

    responseRequired = false;
    responseVisible = false;
    face = PolkitFaceState::None;
    faceConfirm = false;
    faceConfirmIsPrompt = false;
    inputPrompt.clear();
    supplementaryMessage = i18n::tr("auth.polkit.invalid-password");
    supplementaryError = true;
    emitStateChanged();

    if (!startConversation()) {
      kLog.warn("polkit session restart failed for action \"{}\"", pending->actionId);
      clearPending(kErrorFailed, "Failed to restart authentication session");
    }
  }

  void cancelFromAuthority(const std::string& cookie) {
    if (!fromAuthority() || pending == nullptr || pending->cookie != cookie) {
      return;
    }
    clearPending(kErrorCancelled, "Authentication request cancelled by polkit");
  }

  void submitResponse(const std::string& response) {
    if (pending == nullptr || conversation == nullptr || !responseRequired) {
      return;
    }
    // Empty responses make pam_unix report "conversation failed" / "auth could not identify password"; only the
    // pam_gaze on-request prompt takes one (it asks for a face scan).
    if (response.empty() && !faceOnRequest) {
      return;
    }
    faceOnRequest = false;
    sendAnswer(response);
    faceConfirm = false;
    faceConfirmIsPrompt = false;
    responseRequired = false;
    inputPrompt.clear();
    supplementaryMessage = i18n::tr("auth.polkit.authenticating");
    supplementaryError = false;
    emitStateChanged();
  }

  void cancelRequest() {
    if (pending == nullptr) {
      return;
    }
    clearPending(kErrorCancelled, "Authentication request cancelled by user");
  }

  void addPollFds(std::vector<pollfd>& fds) const {
    polledFd = conversation != nullptr ? conversation->fd() : -1;
    if (polledFd >= 0) {
      fds.push_back({.fd = polledFd, .events = POLLIN, .revents = 0});
    }
  }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (conversation == nullptr || polledFd != conversation->fd() || startIdx >= fds.size() ||
        fds[startIdx].revents == 0) {
      return;
    }
    std::vector<std::string> lines;
    const bool open = conversation->readLines(lines);
    const std::uint64_t serial = conversationSerial;
    for (const std::string& line : lines) {
      if (conversationSerial != serial) {
        return;
      }
      handleHelperLine(line);
    }
    if (!open && conversationSerial == serial) {
      kLog.warn("polkit helper ended without a result");
      clearPending(kErrorFailed, "Authentication helper failed");
    }
  }

  PolkitRequest pendingRequest() const {
    if (pending == nullptr) {
      return {};
    }
    return PolkitRequest{
        .actionId = pending->actionId,
        .message = pending->message,
        .iconName = pending->iconName,
        .cookie = pending->cookie,
        .isInternal = pending->isInternal,
        .identities = pending->identities,
        .activeUid = pending->activeUid,
    };
  }
};

PolkitAgent::PolkitAgent(SystemBus& bus) : m_impl(std::make_unique<Impl>(bus)), m_bus(bus) { registerWithGaze(); }

void PolkitAgent::registerWithGaze() {
  const sdbus::ServiceName gazeName{"com.gundulabs.Gaze"};
  const auto add = [this]() {
    try {
      m_gaze->callMethodAsync("AddPamInternal")
          .onInterface("com.gundulabs.Gaze")
          .withArguments(std::string{"polkit-1"})
          .uponReplyInvoke([](std::optional<sdbus::Error> e) {
            if (e.has_value()) {
              kLog.debug("gazed did not take polkit-1: {}", e->what());
              return;
            }
            kLog.info("gazed sends face prompts to this agent");
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("could not reach gazed: {}", e.what());
    }
  };
  try {
    m_gaze = sdbus::createProxy(m_bus.connection(), gazeName, sdbus::ObjectPath{"/com/gundulabs/Gaze"});
    m_dbus = sdbus::createProxy(
        m_bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
    );
    using ScoredFace = sdbus::Struct<std::string, double, double, bool, double, double, bool>;
    m_gaze->uponSignal("VerifyStatus")
        .onInterface("com.gundulabs.Gaze")
        .call([this, gazeName](
                  const std::string& result, const std::vector<ScoredFace>& faces, const std::string& rgb,
                  const std::string& ir
              ) {
          if (result == "verify-match" || !m_bus.isNameOwner(gazeName, m_gaze->getCurrentlyProcessedMessage().getSender())) {
            return;
          }
          kLog.info("face check ended: {} judged={} rgb={} ir={}", result, !faces.empty(), rgb, ir);
          const char* key = !faces.empty()                                     ? "auth.face.no-match"
              : rgb == "too-dark" && (ir == "too-dark" || ir == "unused") ? "auth.face.too-dark"
                                                                            : "auth.face.no-face";
          m_impl->faceCheckEnded(i18n::tr(key));
        });
    // gazed keeps the list in memory, so a restarted gazed needs it again.
    m_dbus->uponSignal("NameOwnerChanged")
        .onInterface("org.freedesktop.DBus")
        .call([add, gazeName](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
          if (name == gazeName && !newOwner.empty()) {
            add();
          }
        });
    if (m_bus.nameHasOwner(gazeName)) {
      add();
    }
  } catch (const std::exception& e) {
    kLog.debug("face prompts off: {}", e.what());
  }
}

PolkitAgent::~PolkitAgent() = default;

void PolkitAgent::start() { m_impl->start(); }

void PolkitAgent::setStateCallback(StateCallback callback) { m_impl->stateCallback = std::move(callback); }

void PolkitAgent::setReadyCallback(ReadyCallback callback) { m_impl->readyCallback = std::move(callback); }

void PolkitAgent::submitResponse(const std::string& response) { m_impl->submitResponse(response); }
void PolkitAgent::selectIdentity(std::uint32_t uid) { m_impl->selectIdentity(uid); }

void PolkitAgent::confirmFace() { m_impl->confirmFace(); }

void PolkitAgent::cancelRequest() { m_impl->cancelRequest(); }

void PolkitAgent::markNextRequestInternal() { m_impl->m_nextInternal = true; }

void PolkitAgent::addPollFds(std::vector<pollfd>& fds) const { m_impl->addPollFds(fds); }

int PolkitAgent::pollTimeoutMs() const { return -1; }

void PolkitAgent::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) { m_impl->dispatch(fds, startIdx); }

bool PolkitAgent::hasPendingRequest() const noexcept { return m_impl->pending != nullptr; }

PolkitRequest PolkitAgent::pendingRequest() const { return m_impl->pendingRequest(); }

bool PolkitAgent::isResponseRequired() const noexcept { return m_impl->responseRequired; }

bool PolkitAgent::responseVisible() const noexcept { return m_impl->responseVisible; }

std::string PolkitAgent::inputPrompt() const { return m_impl->inputPrompt; }

std::string PolkitAgent::supplementaryMessage() const { return m_impl->supplementaryMessage; }

bool PolkitAgent::supplementaryIsError() const noexcept { return m_impl->supplementaryError; }

PolkitFaceState PolkitAgent::faceState() const noexcept { return m_impl->face; }

bool PolkitAgent::faceConfirmPending() const noexcept { return m_impl->faceConfirm && m_impl->responseRequired; }

bool PolkitAgent::faceConfirmIsPrompt() const noexcept { return m_impl->faceConfirmIsPrompt; }

bool PolkitAgent::faceOnRequestPending() const noexcept { return m_impl->faceOnRequest && m_impl->responseRequired; }
