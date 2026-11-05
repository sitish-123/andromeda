#pragma once

#include "app/desk_app.h"
#include "core/timer_manager.h"
#include "mpv_player.h"
#include "render/core/async_texture_cache.h"
#include "ui/controls/scroll_view.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class Button;
class Renderer;
class Label;
class MprisServer;
class MusicLibrary;
class Slider;
class VideoSurface;
struct zwp_idle_inhibitor_v1;

// andromeda-player: one window, one libmpv core (loaded on the first play), MPRIS, resume position per file.
class PlayerApp final : public DeskApp {
public:
  PlayerApp();
  ~PlayerApp() override;

protected:
  void startup() override;
  void open(const std::vector<std::string>& uris, const std::string& activationToken) override;
  void action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) override;
  AppWindow& createWindow() override;
  void windowClosed(AppWindow& window) override;
  void shutdown() override { saveResume(); }

private:
  void play(const std::string& path);
  void togglePip();
  void playAlbum(const std::string& title, const std::string& artist);
  void playPlaylist(const std::string& name, std::size_t start);
  enum class LibraryView : std::uint8_t { NowPlaying, Albums, Artists, Songs, Playlists };
  [[nodiscard]] bool libraryShown() const;
  void updateSidebar();
  void showView(LibraryView view);
  bool libraryBack();
  void playAlbumAt(std::size_t album, std::size_t index);
  void playSongsAt(std::size_t index);
  void playAlbumShuffled(std::size_t album);
  void playQueue(const std::vector<std::string>& paths, bool albumGain, std::size_t start);
  [[nodiscard]] std::unique_ptr<Node> buildLibrary(Renderer& renderer, float scale, float width);
  [[nodiscard]] std::unique_ptr<Node> buildNowPlaying(Renderer& renderer, float scale);
  [[nodiscard]] std::unique_ptr<Node> buildArt(Renderer& renderer, const std::filesystem::path& path, float size, float scale);
  [[nodiscard]] AppWindow* videoHost() const;
  void releaseInhibitor();
  [[nodiscard]] std::unique_ptr<Node> buildPipContent(float width, float height);
  void onPlayerChanged(bool seeked);
  void saveResume();
  void updateInhibit();
  void placeVideo();
  void showControls();
  void updateTransport();
  [[nodiscard]] std::unique_ptr<Node> buildContent(Renderer& renderer, float width, float height);
  [[nodiscard]] static std::string resumeKey(const std::string& path);

  MpvPlayer m_mpv;
  std::unique_ptr<MprisServer> m_mpris;
  std::unique_ptr<VideoSurface> m_video;
  AsyncTextureCache m_artCache;
  std::string m_looseArtPath;
  std::filesystem::path m_looseArt;
  AppWindow* m_window = nullptr;
  std::unique_ptr<MusicLibrary> m_library;
  ScrollViewState m_libraryScroll;
  LibraryView m_view = LibraryView::Albums;
  std::optional<std::size_t> m_openAlbum;
  std::optional<std::size_t> m_openPlaylist;
  std::string m_openArtist;
  bool m_lastHasVideo = false;
  std::string m_nowPlayingPath;
  AppWindow* m_pip = nullptr; // the picture-in-picture window while the video is in it
  zwp_idle_inhibitor_v1* m_inhibitor = nullptr;
  std::string m_pending;           // a file opened before the window exists
  bool m_controlsVisible = true;
  Timer m_hideControls;
  std::int64_t m_savedAtSecond = -1;
  std::int64_t m_shownSecond = -1;
  bool m_lastPaused = true;
  bool m_lastIdle = true;
  int m_inhibitInputs = -1;
  struct Resume {
    std::string path;
    double position = 0.0;
    double duration = 0.0;
    bool eof = false;
  };
  Resume m_resume; // the last loaded file's position, kept past Stop
  Label* m_time = nullptr;
  Label* m_remaining = nullptr;
  Slider* m_seek = nullptr;
};
