#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

class ClipboardService;
class ConfigService;
class HttpClient;

// GIFs or stickers from the Klipy API (shell.launcher.gif_api_key); stickers add the Fluent 3D pack (MIT). Results and thumbnails load asynchronously;
// thumbnails and pasted files are cached in $XDG_CACHE_HOME/andromeda/klipy.
class KlipyProvider : public LauncherProvider {
public:
  enum class Kind : std::uint8_t { Gifs, Stickers };

  KlipyProvider(Kind kind, ConfigService* config, HttpClient* http, ClipboardService* clipboard);
  ~KlipyProvider() override { *m_alive = false; }
  KlipyProvider(const KlipyProvider&) = delete;
  KlipyProvider& operator=(const KlipyProvider&) = delete;

  [[nodiscard]] std::string_view defaultPrefix() const override { return m_kind == Kind::Gifs ? "gif" : "sticker"; }
  [[nodiscard]] std::string_view id() const override { return m_kind == Kind::Gifs ? "Gifs" : "Stickers"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return m_kind == Kind::Gifs ? "gif" : "sticker"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }
  [[nodiscard]] bool isLoading() const override { return m_loading; }
  [[nodiscard]] std::optional<std::string> answeredQuery() const override { return m_answered; }

  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }
  void setActivationDoneCallback(std::function<void(std::string, bool)> callback) override {
    m_onActivationDone = std::move(callback);
  }

  void reset() override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  // Local path of the small animated GIF for a GIF result; empty while it downloads (results refresh when it lands).
  [[nodiscard]] std::string previewGif(const LauncherResult& result);

private:
  struct Media {
    std::string url;
    std::string mime;
    std::string previewUrl;
  };

  struct FluentSticker {
    std::string name; // the repository folder, e.g. "Grinning face"
    std::string path; // repository path of the 3D PNG
  };

  void fetch(const std::string& text) const;
  void loadFluentIndex() const;
  void scheduleRefresh() const;
  [[nodiscard]] std::vector<LauncherResult> fluentResults(const std::string& needle) const;
  void onResponse(const std::string& text, const std::string& body) const;
  [[nodiscard]] std::string customerId() const;

  Kind m_kind;
  ConfigService* m_config = nullptr;
  HttpClient* m_http = nullptr;
  ClipboardService* m_clipboard = nullptr;
  std::filesystem::path m_cacheDir;
  std::function<void()> m_onResultsChanged;
  std::function<void(std::string, bool)> m_onActivationDone;
  // False once the provider is destroyed, so pending HTTP callbacks can bail out.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  mutable Timer m_debounce;
  mutable Timer m_refresh;
  mutable std::unordered_set<std::string> m_missingThumbs;
  std::unordered_set<std::string> m_previewDownloads;
  mutable std::optional<std::string> m_answered;
  mutable std::optional<std::string> m_requested;
  mutable bool m_loading = false;
  mutable std::vector<LauncherResult> m_results;
  mutable std::unordered_map<std::string, Media> m_media;
  mutable bool m_fluentRequested = false;
  mutable std::vector<FluentSticker> m_fluent;
  mutable std::unordered_set<std::string> m_fluentDownloads;
};
