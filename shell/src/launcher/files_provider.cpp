#include "launcher/files_provider.h"

#include "clipboard/clipboard_service.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "util/string_utils.h"

#include <glib.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>

namespace {

  constexpr Logger kLog("files");
  // docs/search.md debounce for async providers: longer than key repeat (25/s = 40 ms), short enough to feel instant.
  constexpr auto kTypingDebounce = std::chrono::milliseconds(60);

  std::string fileUri(const std::string& path) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return {};
    }
    std::string out = uri;
    g_free(uri);
    return out;
  }

  std::string socketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime == nullptr || runtime[0] == '\0' ? std::string() : std::string(runtime) + "/andromeda-index.sock";
  }

  bool isImage(std::string_view path) {
    constexpr std::array<std::string_view, 9> kImages = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".svg", ".jxl", ".avif"};
    std::string lower(path.substr(path.size() > 6 ? path.size() - 6 : 0));
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::any_of(kImages, [&](std::string_view ext) { return lower.ends_with(ext); });
  }

  // The freedesktop thumbnail of a file ($XDG_CACHE_HOME/thumbnails/{x-large,large,normal}/<md5 of uri>.png), or empty.
  std::string cachedThumbnail(const std::string& path) {
    const std::string uri = fileUri(path);
    if (uri.empty()) {
      return {};
    }
    gchar* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), -1);
    const std::string name = std::string(md5) + ".png";
    g_free(md5);
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = cache != nullptr && cache[0] != '\0'
        ? std::filesystem::path(cache)
        : std::filesystem::path(home != nullptr ? home : "") / ".cache";
    std::error_code ec;
    for (const char* size : {"x-large", "large", "normal"}) {
      const auto file = base / "thumbnails" / size / name;
      if (std::filesystem::exists(file, ec)) {
        return file.string();
      }
    }
    return {};
  }

  // The folder as the user reads it: the home folder as "~".
  std::string displayFolder(const std::string& path) {
    std::string folder = std::filesystem::path(path).parent_path().string();
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      const std::string_view h(home);
      if (folder == h) {
        return "~";
      }
      if (folder.starts_with(h) && folder.size() > h.size() && folder[h.size()] == '/') {
        return "~" + folder.substr(h.size());
      }
    }
    return folder;
  }

} // namespace

FilesProvider::FilesProvider(ClipboardService* clipboard, SessionBus* bus) : m_clipboard(clipboard), m_bus(bus) {}

FilesProvider::~FilesProvider() { disconnect(); }

std::string FilesProvider::displayName() const { return i18n::tr("launcher.files.title"); }

bool FilesProvider::connect() const {
  if (m_fd >= 0) {
    return true;
  }
  const std::string path = socketPath();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    kLog.debug("andromeda-index not reachable: {}", std::strerror(errno));
    close(fd);
    return false;
  }
  m_fd = fd;
  return true;
}

void FilesProvider::disconnect() const {
  if (m_fd >= 0) {
    close(m_fd);
  }
  m_fd = -1;
  m_received.clear();
  m_pending.clear();
  m_requested.reset();
  m_loading = false;
  m_debounce.stop();
}

void FilesProvider::reset() {
  m_debounce.stop();
  m_requested.reset();
  m_answered.reset();
  m_results.clear();
  m_loading = false;
}

std::vector<LauncherResult> FilesProvider::query(std::string_view text) const { return request(text, false); }

std::vector<LauncherResult> FilesProvider::queryPrefixed(std::string_view text) const { return request(text, true); }

std::vector<LauncherResult> FilesProvider::request(std::string_view text, bool scoped) const {
  // The broker matches answers to the text it asked, so the raw text is the key; the daemon gets it trimmed.
  const Request key{std::string(text), scoped ? std::optional(m_scopeKind) : std::nullopt};
  std::string needle = StringUtils::trim(text);
  std::erase(needle, '\n');
  if (needle.size() < kMinQuery && (!scoped || m_scopeKind.empty() || !needle.empty())) {
    m_answered = key.first;
    m_answeredScope = key.second;
    m_requested.reset();
    m_results.clear();
    m_loading = false;
    m_debounce.stop();
    return {};
  }
  if (m_answered == key.first && m_answeredScope == key.second) {
    // Back to an answered query: an answer still on its way is for another text now.
    m_requested.reset();
    m_loading = false;
    m_debounce.stop();
    return m_results;
  }
  // Until the new answer arrives, the last one stays, so the section does not blink out on each keystroke.
  if (m_requested == key) {
    return m_results;
  }
  const std::string kind = scoped && !m_scopeKind.empty() ? "kind:" + m_scopeKind + " " : std::string();
  const std::string line = "query " + std::to_string(scoped ? kScopedLimit : kLimit) + " " + kind + needle + "\n";
  m_requested = key;
  m_loading = true;
  m_debounce.start(needle.empty() ? std::chrono::milliseconds(0) : kTypingDebounce, [this, key, line] { send(key, line); });
  return m_results;
}

void FilesProvider::send(const Request& request, const std::string& line) const {
  // Without the daemon there is no Files section; the next query tries to connect again.
  if (!connect() || ::send(m_fd, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
    disconnect();
    m_answered = request.first;
    m_answeredScope = request.second;
    m_results.clear();
    if (m_onResultsChanged) {
      m_onResultsChanged();
    }
    return;
  }
  m_pending.push_back(request);
}

void FilesProvider::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_fd >= 0) {
    fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
  }
}

void FilesProvider::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_fd < 0 || startIdx >= fds.size() || fds[startIdx].revents == 0) {
    return;
  }
  std::array<char, 16384> buffer{};
  while (true) {
    const ssize_t n = read(m_fd, buffer.data(), buffer.size());
    if (n > 0) {
      m_received.append(buffer.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      // The daemon went away: the open query is answered with nothing, so the section stops loading.
      const std::optional<Request> open = m_requested;
      disconnect();
      if (open.has_value()) {
        onAnswer(*open, {});
      }
      return;
    }
    break;
  }
  // Each answer ends with an empty line; an answer with no rows is that empty line alone.
  while (!m_received.empty() && !m_pending.empty()) {
    std::size_t bodyEnd = 0;
    std::size_t consumed = 0;
    if (m_received.front() == '\n') {
      consumed = 1;
    } else if (const std::size_t end = m_received.find("\n\n"); end != std::string::npos) {
      bodyEnd = end + 1;
      consumed = end + 2;
    } else {
      break;
    }
    const std::string body = m_received.substr(0, bodyEnd);
    m_received.erase(0, consumed);
    const Request request = std::move(m_pending.front());
    m_pending.pop_front();
    if (request == m_requested) {
      onAnswer(request, body);
    }
  }
}

void FilesProvider::onAnswer(const Request& request, const std::string& body) const {
  const auto& [text, scope] = request;
  const bool scoped = scope.has_value();
  m_results.clear();
  std::vector<std::int64_t> mtimes;
  std::size_t start = 0;
  for (std::size_t end = body.find('\n'); end != std::string::npos; end = body.find('\n', start)) {
    const std::string_view row = std::string_view(body).substr(start, end - start);
    start = end + 1;
    // path \t mtime_ns \t size \t f|d \t n|c (name or text match)
    const std::size_t tab = row.find('\t');
    if (tab == std::string_view::npos || row.size() < 4 || row[row.size() - 2] != '\t') {
      continue;
    }
    const std::string path(row.substr(0, tab));
    const bool folder = row.substr(row.size() - 4, 2) == "\td";
    mtimes.push_back(std::atoll(std::string(row.substr(tab + 1)).c_str()));
    LauncherResult r;
    r.id = path;
    r.title = std::filesystem::path(path).filename().string();
    r.subtitle = displayFolder(path);
    r.glyphName = folder ? "folder" : "file";
    r.presentation = folder ? "folder" : "file";
    // Images preview themselves; other files use a thumbnail a file manager already made, if any.
    if (!folder) {
      r.iconPath = isImage(path) ? path : cachedThumbnail(path);
      if (!r.iconPath.empty()) {
        r.presentation = "file-image";
      }
    }
    r.category = displayName();
    m_results.push_back(std::move(r));
  }
  kLog.debug("{} files for \"{}\"{}{}", m_results.size(), text, scoped ? " in the Files scope" : "",
             scoped && !scope->empty() ? " kind " + *scope : std::string());
  if (scoped) {
    std::vector<std::size_t> order(m_results.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::ranges::stable_sort(order, std::greater{}, [&](std::size_t i) { return mtimes[i]; });
    std::vector<LauncherResult> sorted;
    sorted.reserve(order.size());
    for (const std::size_t i : order) {
      sorted.push_back(std::move(m_results[i]));
    }
    m_results = std::move(sorted);
  } else if (!m_results.empty()) {
    LauncherResult all;
    all.id = std::string(kShowAllId);
    all.title = i18n::tr("launcher.files.show-all");
    all.glyphName = "list";
    all.presentation = "action";
    all.category = displayName();
    // Last row of the Files section, whatever the other rows score.
    all.score = std::numeric_limits<double>::lowest();
    m_results.push_back(std::move(all));
  }
  m_answered = text;
  m_answeredScope = scope;
  m_requested.reset();
  m_loading = false;
  if (m_onResultsChanged) {
    m_onResultsChanged();
  }
}

bool FilesProvider::activate(const LauncherResult& result) {
  if (!net::openInBrowser(fileUri(result.id))) {
    kLog.warn("opening {} failed", result.id);
  }
  return true;
}

void FilesProvider::reveal(const LauncherResult& result) const {
  const std::string folder = std::filesystem::path(result.id).parent_path().string();
  if (m_bus == nullptr) {
    (void)net::openInBrowser(fileUri(folder));
    return;
  }
  if (m_fileManager == nullptr) {
    m_fileManager = sdbus::createProxy(
        m_bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
        sdbus::ObjectPath{"/org/freedesktop/FileManager1"}
    );
  }
  m_fileManager->callMethodAsync("ShowItems")
      .onInterface("org.freedesktop.FileManager1")
      .withArguments(std::vector<std::string>{fileUri(result.id)}, std::string())
      .uponReplyInvoke([folder](std::optional<sdbus::Error> error) {
        // No file manager owns FileManager1: open the folder itself.
        if (error.has_value() && !net::openInBrowser(fileUri(folder))) {
          kLog.warn("showing {} failed: {}", folder, error->what());
        }
      });
}

void FilesProvider::copyPath(const LauncherResult& result) const {
  if (m_clipboard != nullptr) {
    (void)m_clipboard->copyText(result.id);
  }
}
