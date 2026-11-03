#include "launcher/klipy_provider.h"

#include "clipboard/clipboard_service.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "shell/panel/panel_manager.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::string_view kIdPrefix = "klipy-";
  constexpr std::string_view kSetupId = "klipy-setup";
  constexpr auto kTypingDebounce = std::chrono::milliseconds(250);
  constexpr auto kRefreshDelay = std::chrono::milliseconds(150);
  constexpr std::string_view kFluentPrefix = "fluent-";
  constexpr std::size_t kMaxFluentResults = 64;
  constexpr std::string_view kFluentTreeUrl =
      "https://api.github.com/repos/microsoft/fluentui-emoji/git/trees/main?recursive=1";
  constexpr std::string_view kFluentRawUrl = "https://raw.githubusercontent.com/microsoft/fluentui-emoji/main/";

  std::filesystem::path cacheRoot() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::filesystem::path(xdg) / "andromeda" / "klipy";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".cache" / "andromeda" / "klipy";
    }
    return std::filesystem::path("/tmp") / "andromeda-klipy";
  }

  // Ids come from the API; keep only characters that are safe in a file name.
  std::string safeName(std::string_view id) {
    std::string out;
    for (const char c : id) {
      if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') {
        out.push_back(c);
      }
    }
    return out;
  }

  std::string fileUrl(const nlohmann::json& file, std::string_view size, std::string_view format) {
    const auto s = file.find(size);
    if (s == file.end() || !s->is_object()) {
      return {};
    }
    const auto f = s->find(format);
    if (f == s->end() || !f->is_object()) {
      return {};
    }
    return f->value("url", std::string());
  }
} // namespace

KlipyProvider::KlipyProvider(Kind kind, ConfigService* config, HttpClient* http, ClipboardService* clipboard)
    : m_kind(kind), m_config(config), m_http(http), m_clipboard(clipboard), m_cacheDir(cacheRoot()) {}

std::string KlipyProvider::displayName() const {
  return i18n::tr(m_kind == Kind::Gifs ? "launcher.providers.gifs.title" : "launcher.providers.stickers.title");
}

// The Fluent index loads at startup, so opening the Stickers tab reads no file.
void KlipyProvider::initialize() {
  if (m_kind == Kind::Stickers && std::filesystem::exists(m_cacheDir / "fluent" / "index.tsv")) {
    loadFluentIndex();
  }
}

void KlipyProvider::reset() {
  m_debounce.stop();
  m_refresh.stop();
  m_answered.reset();
  m_requested.reset();
  m_loading = false;
  m_results.clear();
}

void KlipyProvider::setPack(std::string pack) {
  if (pack != m_pack) {
    m_pack = std::move(pack);
    reset();
  }
}

std::string KlipyProvider::customerId() const {
  const std::filesystem::path path = m_cacheDir / "customer_id";
  std::string id;
  if (std::ifstream in(path); in.is_open()) {
    std::getline(in, id);
  }
  if (id.empty()) {
    id = StringUtils::generateUuid();
    std::error_code ec;
    std::filesystem::create_directories(m_cacheDir, ec);
    std::ofstream(path) << id;
  }
  return id;
}

std::vector<LauncherResult> KlipyProvider::query(std::string_view text) const {
  const std::string needle = StringUtils::trim(text);
  std::vector<LauncherResult> results = m_pack == "klipy" ? std::vector<LauncherResult>{} : fluentResults(needle);
  if (m_pack == "fluent") {
    m_answered = needle;
    return results;
  }
  if (m_config == nullptr || m_http == nullptr || m_config->config().shell.launcher.gifApiKey.empty()) {
    m_answered = needle;
    // Stickers without a key still have the Fluent pack; the key hint lives in the GIFs tab then.
    if (!results.empty()) {
      return results;
    }
    LauncherResult r;
    r.id = std::string(kSetupId);
    r.title = i18n::tr("launcher.klipy.add-key");
    r.subtitle = i18n::tr("launcher.klipy.add-key-hint");
    r.glyphName = "key";
    r.presentation = "action";
    r.category = displayName();
    return {r};
  }
  if (m_answered == needle) {
    // The texture cache keeps a failed load, so a thumbnail path is set only once its file exists.
    std::vector<LauncherResult> klipy = m_results;
    for (LauncherResult& r : klipy) {
      if (m_missingThumbs.contains(r.id)) {
        r.iconPath.clear();
      }
    }
    results.insert(results.begin(), klipy.begin(), klipy.end());
    return results;
  }
  if (m_requested != needle) {
    m_requested = needle;
    m_loading = true;
    m_debounce.start(needle.empty() ? std::chrono::milliseconds(0) : kTypingDebounce, [this, needle] { fetch(needle); });
  }
  return {};
}

void KlipyProvider::fetch(const std::string& text) const {
  const std::string kind = m_kind == Kind::Gifs ? "gifs" : "stickers";
  std::string url = "https://api.klipy.com/api/v1/" + StringUtils::urlEncode(m_config->config().shell.launcher.gifApiKey)
      + "/" + kind + (text.empty() ? "/trending?" : "/search?q=" + StringUtils::urlEncode(text) + "&")
      + "page=1&per_page=24&customer_id=" + customerId();
  m_http->request(HttpRequest{.url = std::move(url)}, [this, alive = m_alive, text](const HttpResponse& response) {
    if (!*alive || m_requested != text) {
      return;
    }
    onResponse(text, response.transportOk && response.status == 200 ? response.body : std::string());
  });
}

void KlipyProvider::onResponse(const std::string& text, const std::string& body) const {
  m_results.clear();
  m_media.clear();
  const bool gifs = m_kind == Kind::Gifs;
  const std::string section = i18n::tr(text.empty() ? "launcher.klipy.trending" : "launcher.pill.results");
  const std::filesystem::path thumbs = m_cacheDir / "thumbs";
  std::error_code ec;
  std::filesystem::create_directories(thumbs, ec);
  try {
    const auto json = nlohmann::json::parse(body);
    for (const auto& item : json.at("data").at("data")) {
      const auto file = item.find("file");
      if (file == item.end()) {
        continue;
      }
      const std::string name = safeName(item.contains("id") ? item["id"].dump() : item.value("slug", ""));
      const std::string thumbUrl = fileUrl(*file, "sm", gifs ? "jpg" : "png");
      const std::string mediaUrl = fileUrl(*file, "md", gifs ? "gif" : "png");
      if (name.empty() || thumbUrl.empty() || mediaUrl.empty()) {
        continue;
      }
      LauncherResult r;
      r.id = std::string(kIdPrefix) + name;
      r.title = item.value("title", std::string());
      r.glyphName = gifs ? "gif" : "sticker";
      r.presentation = gifs ? "gif" : "sticker";
      r.category = section;
      const std::filesystem::path thumb = thumbs / (name + (gifs ? ".jpg" : ".png"));
      r.iconPath = thumb.string();
      if (!std::filesystem::exists(thumb, ec)) {
        m_missingThumbs.insert(r.id);
        m_http->download(thumbUrl, thumb, [this, alive = m_alive, id = r.id](bool ok) {
          if (*alive && ok) {
            m_missingThumbs.erase(id);
            scheduleRefresh();
          }
        });
      }
      m_media[r.id] = {mediaUrl, gifs ? "image/gif" : "image/png", gifs ? fileUrl(*file, "sm", "gif") : std::string()};
      m_results.push_back(std::move(r));
    }
  } catch (const nlohmann::json::exception&) {
    m_results.clear();
    m_media.clear();
  }
  m_answered = text;
  m_loading = false;
  if (m_onResultsChanged) {
    m_onResultsChanged();
  }
}

void KlipyProvider::scheduleRefresh() const {
  // Thumbnails land one by one; refresh at most every kRefreshDelay so the grid does not rebuild per file.
  if (!m_refresh.active()) {
    m_refresh.start(kRefreshDelay, [this] {
      if (m_onResultsChanged) {
        m_onResultsChanged();
      }
    });
  }
}

void KlipyProvider::loadFluentIndex() const {
  m_fluentRequested = true;
  const std::filesystem::path dir = m_cacheDir / "fluent";
  const std::filesystem::path index = dir / "index.tsv";
  if (std::ifstream in(index); in.is_open()) {
    std::string line;
    while (std::getline(in, line)) {
      if (const auto tab = line.find('\t'); tab != std::string::npos) {
        m_fluent.push_back({line.substr(0, tab), line.substr(tab + 1)});
      }
    }
    return;
  }
  // One listing of the repository finds every 3D image; each image downloads when a result first shows it.
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  HttpRequest request{.url = std::string(kFluentTreeUrl), .headers = {"User-Agent: andromeda-shell"}};
  m_http->request(std::move(request), [this, alive = m_alive, dir, index](const HttpResponse& response) {
    if (!*alive || !response.transportOk || response.status != 200) {
      return;
    }
    try {
      for (const auto& entry : nlohmann::json::parse(response.body).at("tree")) {
        const std::string path = entry.value("path", std::string());
        // assets/<Name>/3D/<file>.png, or assets/<Name>/Default/3D/<file>.png for skin-toned emoji.
        if (!path.starts_with("assets/") || !path.ends_with(".png")
            || (!path.contains("/Default/3D/") && path.find("/3D/") != path.find('/', 7))) {
          continue;
        }
        m_fluent.push_back({path.substr(7, path.find('/', 7) - 7), path});
      }
    } catch (const nlohmann::json::exception&) {
      m_fluent.clear();
      return;
    }
    std::ofstream out(index);
    for (const FluentSticker& sticker : m_fluent) {
      out << sticker.name << '\t' << sticker.path << '\n';
    }
    if (m_onResultsChanged) {
      m_onResultsChanged();
    }
  });
}

std::vector<LauncherResult> KlipyProvider::fluentResults(const std::string& needle) const {
  if (m_kind != Kind::Stickers || m_http == nullptr) {
    return {};
  }
  if (!m_fluentRequested) {
    loadFluentIndex();
  }
  const std::filesystem::path dir = m_cacheDir / "fluent";
  const std::string lower = StringUtils::toLower(needle);
  const std::string section = i18n::tr("launcher.klipy.fluent");
  std::vector<LauncherResult> results;
  for (const FluentSticker& sticker : m_fluent) {
    if (results.size() >= kMaxFluentResults) {
      break;
    }
    if (!lower.empty() && !StringUtils::toLower(sticker.name).contains(lower)) {
      continue;
    }
    const std::string file = sticker.path.substr(sticker.path.rfind('/') + 1);
    const std::filesystem::path local = dir / file;
    LauncherResult r;
    r.id = std::string(kFluentPrefix) + file;
    r.title = sticker.name;
    r.glyphName = "sticker";
    r.presentation = "sticker";
    r.category = section;
    std::error_code ec;
    if (std::filesystem::exists(local, ec)) {
      r.iconPath = local.string();
    } else if (!m_fluentDownloads.contains(file)) {
      m_fluentDownloads.insert(file);
      std::string url(kFluentRawUrl);
      for (std::size_t start = 0; start < sticker.path.size();) {
        const std::size_t end = std::min(sticker.path.find('/', start), sticker.path.size());
        url += (start == 0 ? "" : "/") + StringUtils::urlEncode(std::string_view(sticker.path).substr(start, end - start));
        start = end + 1;
      }
      m_http->download(url, local, [this, alive = m_alive, file](bool ok) {
        if (*alive) {
          if (!ok) {
            m_fluentDownloads.erase(file);
          }
          scheduleRefresh();
        }
      });
    }
    results.push_back(std::move(r));
  }
  return results;
}

std::string KlipyProvider::previewGif(const LauncherResult& result) {
  const auto media = m_media.find(result.id);
  if (media == m_media.end() || media->second.previewUrl.empty() || m_http == nullptr) {
    return {};
  }
  const std::filesystem::path path = m_cacheDir / "preview" / (result.id.substr(kIdPrefix.size()) + ".gif");
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return path.string();
  }
  if (m_previewDownloads.insert(result.id).second) {
    std::filesystem::create_directories(path.parent_path(), ec);
    m_http->download(media->second.previewUrl, path, [this, alive = m_alive, id = result.id](bool ok) {
      if (*alive) {
        if (!ok) {
          m_previewDownloads.erase(id);
        }
        scheduleRefresh();
      }
    });
  }
  return {};
}

bool KlipyProvider::activate(const LauncherResult& result) {
  if (result.id == kSetupId) {
    PanelManager::instance().openSettingsWindow("launcher");
    // Close through the done callback with nothing copied, so auto-paste does not fire.
    if (m_onActivationDone) {
      m_onActivationDone(result.id, false);
    }
    return false;
  }
  if (result.id.starts_with(kFluentPrefix)) {
    auto png = FileUtils::readBinaryFile(result.iconPath);
    return m_clipboard != nullptr && m_clipboard->copyImagePng(std::move(png));
  }
  const auto media = m_media.find(result.id);
  if (m_clipboard == nullptr || media == m_media.end()) {
    return false;
  }
  const std::filesystem::path path = m_cacheDir / "media"
      / (result.id.substr(kIdPrefix.size()) + (m_kind == Kind::Gifs ? ".gif" : ".png"));
  const auto copy = [this, path, mime = media->second.mime, url = media->second.url] {
    auto bytes = FileUtils::readBinaryFile(path.string());
    return bytes.empty() ? m_clipboard->copyText(url)
                         : m_clipboard->copyText(std::string(bytes.begin(), bytes.end()), mime);
  };
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return copy();
  }
  std::filesystem::create_directories(path.parent_path(), ec);
  // The launcher stays open until the file is on disk; the done callback closes it and runs auto-paste.
  m_http->download(media->second.url, path, [this, alive = m_alive, copy, id = result.id](bool) {
    if (*alive && m_onActivationDone) {
      m_onActivationDone(id, copy());
    }
  });
  return false;
}
