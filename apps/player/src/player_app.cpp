#include "player_app.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "mpris_server.h"
#include "music_library.h"
#include "render/backend/render_backend.h"
#include "ui/controls/scroll_view.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "video_surface.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <cmath>
#include <algorithm>
#include <random>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("player");
  constexpr float kBarHeight = 56.0F;
  constexpr float kAlbumTile = 160.0F;
  constexpr auto kControlsTimeout = std::chrono::milliseconds(2500);
  constexpr std::uint32_t kDoubleClickMs = 400;
  constexpr double kResumeMinimum = 10.0; // seconds in before a position is worth keeping
  constexpr double kResumeTail = 15.0;    // seconds before the end that count as finished
  constexpr std::int64_t kSaveEvery = 5;  // seconds of playback between saves

  std::string clock(double seconds) {
    const auto total = static_cast<std::int64_t>(std::max(0.0, std::floor(seconds)));
    const auto h = total / 3600;
    const auto m = total / 60 % 60;
    const auto s = total % 60;
    return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
  }

  std::string pathFromUri(const std::string& uri) {
    if (uri.starts_with("file://")) {
      gchar* file = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
      std::string out = file != nullptr ? file : uri;
      g_free(file);
      return out;
    }
    return uri;
  }
} // namespace

PlayerApp::PlayerApp() : DeskApp({.id = "dev.andromeda.Player", .name = "Player"}) {}

PlayerApp::~PlayerApp() {
  if (m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
  }
  // The album images hold textures of m_artCache: the windows go before it, the video before its windows.
  m_video.reset();
  destroyWindows();
  loop().removeSource(&m_artCache);
}

void PlayerApp::startup() {
  loop().addSource(&m_mpv);
  m_artCache.initialize(&loop().gl());
  m_artCache.setMakeCurrentCallback([this] { (void)loop().renderContext().backend().makeCurrentNoSurface(); });
  loop().addSource(&m_artCache);
  m_mpv.setChangedCallback([this](bool seeked) { onPlayerChanged(seeked); });
  if (bus() != nullptr) {
    m_mpris = std::make_unique<MprisServer>(
        *bus(), m_mpv,
        MprisServer::Actions{
            .raise = [this] {
              if (m_window != nullptr) {
                m_window->activate({});
              }
            },
            .quit = [this] { quit(0); },
            .openUri = [this](const std::string& uri) { play(pathFromUri(uri)); },
        }
    );
  }
  commands().add({.id = "player.play-pause", .label = "Play/Pause", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_space, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.togglePause(); }});
  commands().add({.id = "player.forward", .label = "Forward 10 Seconds", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_Right, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.seekBy(10.0); }});
  commands().add({.id = "player.back", .label = "Back 10 Seconds", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_Left, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.seekBy(-10.0); }});
  const char* music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
  const std::filesystem::path root = music != nullptr ? music : std::filesystem::path(g_get_home_dir()) / "Music";
  m_library = std::make_unique<MusicLibrary>(root, std::filesystem::path(g_get_user_cache_dir()) / "andromeda/player/library.json");
  m_library->scan([this] {
    updateSidebar();
    if (m_window != nullptr) {
      m_window->requestRebuild();
    }
  });
  commands().add({.id = "go.back", .label = "Back", .group = "Go",
                  .shortcut = KeyChord{.sym = XKB_KEY_Left, .modifiers = KeyMod::Alt},
                  .enabled = [this] { return m_openAlbum.has_value() || m_openPlaylist.has_value() || !m_openArtist.empty(); },
                  .run = [this] { libraryBack(); }});
  commands().add({.id = "go.forward", .label = "Forward", .group = "Go", .shortcut = std::nullopt,
                  .enabled = [] { return false; }, .run = [] {}});
  commands().add({.id = "player.next", .label = "Next", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_n, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.next(); }});
  commands().add({.id = "player.previous", .label = "Previous", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_b, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.previous(); }});
  commands().add({.id = "view.pip", .label = "Picture in Picture", .group = "View",
                  .shortcut = KeyChord{.sym = XKB_KEY_p, .modifiers = 0}, .enabled = {},
                  .run = [this] { DeferredCall::callLater([this] { togglePip(); }); }});
  commands().add({.id = "player.subtitles", .label = "Next Subtitle Track", .group = "Playback",
                  .shortcut = KeyChord{.sym = XKB_KEY_c, .modifiers = 0}, .enabled = {},
                  .run = [this] { m_mpv.cycleSubtitles(); }});
  commands().add({.id = "view.fullscreen", .label = "Full Screen", .group = "View",
                  .shortcut = KeyChord{.sym = XKB_KEY_f, .modifiers = 0}, .enabled = {},
                  .run = [this] {
                    if (m_window != nullptr) {
                      m_window->toggleFullscreen();
                    }
                  }});
}

AppWindow& PlayerApp::createWindow() {
  AppWindow& window = addWindow({
      .appId = info().id,
      .title = info().name,
      .width = 960,
      .height = 600,
      .minWidth = 320,
      .minHeight = 240,
      .navigation = true,
      .sidebarWidth = 200.0F,
      .transparentContent = true,
      .headerWhenFullscreen = false,
  });
  m_window = &window;
  updateSidebar();
  window.setContentBuilder([this](Renderer& renderer, float width, float height) { return buildContent(renderer, width, height); });
  window.setLayoutCallback([this] { placeVideo(); });
  return window;
}

void PlayerApp::windowClosed(AppWindow& window) {
  if (&window != m_window && &window != m_pip) {
    return;
  }
  // The inhibitor and the video belong to the closing surface; they move to the window that stays, if any.
  releaseInhibitor();
  if (&window == m_pip) {
    m_pip = nullptr;
  } else {
    saveResume();
    m_window = nullptr;
    m_time = m_remaining = nullptr;
    m_seek = nullptr;
  }
  AppWindow* host = videoHost();
  if (m_video != nullptr && (host == nullptr || !m_video->reparent(host->wlSurface()))) {
    m_video.reset();
  }
  if (host != nullptr) {
    host->requestRebuild();
  }
  updateInhibit();
}

AppWindow* PlayerApp::videoHost() const { return m_pip != nullptr ? m_pip : m_window; }

void PlayerApp::releaseInhibitor() {
  if (m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
  }
}

void PlayerApp::togglePip() {
  if (m_pip != nullptr) {
    // Back to a window: open the main one again if the user closed it, so the video has a home and the app stays.
    if (m_window == nullptr) {
      DeskApp::activate({});
    }
    closeWindow(*m_pip);
    return;
  }
  if (m_video == nullptr || m_mpv.state().idle) {
    return;
  }
  AppWindow& pip = addWindow({
      .appId = info().id + ".PiP",
      .title = "Picture in Picture",
      .stateKey = "pip",
      .width = 400,
      .height = 225,
      .minWidth = 192,
      .minHeight = 108,
      .transparentContent = true,
      .header = false,
  });
  m_pip = &pip;
  pip.setContentBuilder([this](Renderer& /*renderer*/, float width, float height) { return buildPipContent(width, height); });
  pip.setLayoutCallback([this] { placeVideo(); });
  if (!pip.open()) {
    closeWindow(pip);
    return;
  }
  releaseInhibitor();
  if (!m_video->reparent(pip.wlSurface())) {
    m_video.reset();
  }
  kLog.debug("picture in picture on");
  if (m_window != nullptr) {
    m_window->requestRebuild();
  }
  updateInhibit();
}

std::unique_ptr<Node> PlayerApp::buildPipContent(float width, float height) {
  const float s = m_pip->uiScale();
  const MpvPlayer::State& st = m_mpv.state();
  auto area = ui::inputArea({});
  area->setSize(width, height);
  area->setOnPress([this, last = std::uint32_t{0}](const InputArea::PointerData& data) mutable {
    if (!data.pressed || data.button != BTN_LEFT) {
      return;
    }
    // Double-click returns to the window; a single click toggles pause.
    if (last != 0 && data.time - last < kDoubleClickMs) {
      last = 0;
      m_mpv.togglePause();
      DeferredCall::callLater([this] { togglePip(); });
      return;
    }
    last = data.time;
    m_mpv.togglePause();
  });
  area->setOnMotion([this](const InputArea::PointerData&) { showControls(); });
  if (m_controlsVisible) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * s, .padding = Style::spaceXs * s});
    row->addChild(ui::node({.flexGrow = 1.0F}));
    row->addChild(ui::button({.glyph = std::string(st.paused ? "player-play" : "player-pause"), .glyphSize = 16.0F * s,
                              .variant = ButtonVariant::Secondary, .onClick = [this] { m_mpv.togglePause(); }}));
    row->addChild(ui::button({.glyph = std::string("picture-in-picture-off"), .glyphSize = 16.0F * s,
                              .variant = ButtonVariant::Secondary,
                              .onClick = [this] { DeferredCall::callLater([this] { togglePip(); }); }}));
    row->setSize(width, 40.0F * s);
    area->addChild(std::move(row));
  }
  return area;
}

void PlayerApp::open(const std::vector<std::string>& uris, const std::string& activationToken) {
  DeskApp::activate(activationToken);
  if (!uris.empty()) {
    play(pathFromUri(uris.front()));
  }
}

void PlayerApp::action(const std::string& name, const std::vector<std::string>& parameter, const std::string& activationToken) {
  if (name == "seek" && !parameter.empty()) {
    m_mpv.seekTo(std::strtod(parameter.front().c_str(), nullptr));
    return;
  }
  if (name == "library.play-album" && !parameter.empty()) {
    playAlbum(parameter.front(), parameter.size() > 1 ? parameter[1] : std::string());
    return;
  }
  if (name == "library.show-album" && !parameter.empty() && m_library != nullptr) {
    if (const MusicAlbum* album = m_library->findAlbum(parameter.front())) {
      showView(LibraryView::Albums);
      m_openAlbum = static_cast<std::size_t>(album - m_library->albums().data());
    }
    return;
  }
  if (name == "library.play-playlist" && !parameter.empty()) {
    playPlaylist(parameter.front(), 0);
    return;
  }
  DeskApp::action(name, parameter, activationToken);
}

std::string PlayerApp::resumeKey(const std::string& path) {
  gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path.c_str(), -1);
  std::string out = std::format("/resume/{}", sum);
  g_free(sum);
  return out;
}

void PlayerApp::play(const std::string& path) {
  if (!m_mpv.initialize()) {
    kLog.warn("libmpv is not available");
    return;
  }
  saveResume();
  m_resume = {};
  const double start = state().get<double>(resumeKey(path) + "/pos", 0.0);
  kLog.info("play {} from {:.1f} s", path, start);
  m_savedAtSecond = -1;
  // vo=libmpv needs the render context before the file opens its video output, or mpv turns video off.
  placeVideo();
  m_mpv.load(path, start);
  if (m_window != nullptr) {
    m_window->requestRebuild();
  }
}

void PlayerApp::playAlbum(const std::string& title, const std::string& artist) {
  const MusicAlbum* album = m_library != nullptr ? m_library->findAlbum(title, artist) : nullptr;
  if (album == nullptr) {
    kLog.warn("no album {}", title);
    return;
  }
  playAlbumAt(static_cast<std::size_t>(album - m_library->albums().data()), 0);
}

void PlayerApp::playPlaylist(const std::string& name, std::size_t start) {
  const MusicPlaylist* list = m_library != nullptr ? m_library->findPlaylist(name) : nullptr;
  if (list == nullptr || list->entries.empty()) {
    kLog.warn("no playlist {}", name);
    return;
  }
  std::vector<std::string> paths;
  for (const auto& entry : list->entries) {
    paths.push_back(entry.string());
  }
  // A playlist mixes albums: per-track gain.
  playQueue(paths, false, start);
}

void PlayerApp::saveResume() {
  // From the last loaded state: after Stop or an error end mpv is idle and has dropped the path.
  const Resume& r = m_resume;
  if (r.path.empty() || r.duration <= 0.0) {
    return;
  }
  const std::string key = resumeKey(r.path);
  if (r.eof || r.position < kResumeMinimum || r.position > r.duration - kResumeTail) {
    state().set(key, nullptr);
  } else {
    state().set(key, {{"pos", r.position}, {"at", static_cast<std::int64_t>(std::time(nullptr))}});
  }
  kLog.debug("resume {} at {:.1f} s", r.path, r.position);
}

void PlayerApp::onPlayerChanged(bool seeked) {
  const MpvPlayer::State& s = m_mpv.state();
  if (!s.idle && !s.path.empty() && s.duration > 0.0) {
    m_resume = {.path = s.path, .position = s.position, .duration = s.duration, .eof = s.eof};
  }
  if (!s.idle && s.path != m_nowPlayingPath) {
    m_nowPlayingPath = s.path;
    // mpv reports the new path before its media-title: the library knows the title already.
    std::string title;
    if (m_library != nullptr) {
      for (const MusicTrack& t : m_library->tracks()) {
        if (t.path == s.path) {
          title = t.title;
          break;
        }
      }
    }
    kLog.info("now playing {}", !title.empty() ? title : s.path);
    if (m_window != nullptr) {
      m_window->requestRebuild();
    }
  }
  if (m_mpris != nullptr) {
    m_mpris->update(seeked);
  }
  if (!s.paused && !s.idle) {
    if (const auto second = static_cast<std::int64_t>(s.position) / kSaveEvery; second != m_savedAtSecond) {
      m_savedAtSecond = second;
      saveResume();
    }
  }
  updateInhibit();
  if (s.hasVideo != m_lastHasVideo) {
    m_lastHasVideo = s.hasVideo;
    updateSidebar();
    if (m_window != nullptr) {
      m_window->requestRebuild();
    }
  }
  if (s.paused != m_lastPaused || s.idle != m_lastIdle) {
    m_lastPaused = s.paused;
    m_lastIdle = s.idle;
    if (s.paused) {
      saveResume();
    }
    showControls();
    if (m_window != nullptr) {
      m_window->requestRebuild();
    }
    if (m_pip != nullptr) {
      m_pip->requestRebuild();
    }
    return;
  }
  updateTransport();
}

void PlayerApp::updateTransport() {
  const MpvPlayer::State& s = m_mpv.state();
  const auto second = static_cast<std::int64_t>(s.position);
  // Text and slider move once per second: no relayout per video frame.
  if (m_window == nullptr || m_time == nullptr || second == m_shownSecond) {
    return;
  }
  m_shownSecond = second;
  m_time->setText(clock(s.position));
  m_remaining->setText("-" + clock(s.duration - s.position));
  if (m_seek != nullptr && s.duration > 0.0) {
    m_seek->setValue(s.position / s.duration);
  }
  m_window->invalidate();
}

void PlayerApp::updateInhibit() {
  const MpvPlayer::State& s = m_mpv.state();
  // Video in front only; audio alone never keeps the screen on (docs/apps.md §5).
  AppWindow* host = videoHost();
  const bool want = host != nullptr && s.hasVideo && !s.paused && !s.idle && !s.eof;
  if (const int inputs = (host != nullptr) | s.hasVideo << 1 | s.paused << 2 | s.idle << 3 | s.eof << 4;
      inputs != m_inhibitInputs) {
    m_inhibitInputs = inputs;
    kLog.debug("inhibit want={} video={} paused={} idle={} eof={} manager={}", want, s.hasVideo, s.paused, s.idle, s.eof,
               loop().wayland().idleInhibitManager() != nullptr);
  }
  zwp_idle_inhibit_manager_v1* manager = loop().wayland().idleInhibitManager();
  if (want && m_inhibitor == nullptr && manager != nullptr && host->wlSurface() != nullptr) {
    m_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(manager, host->wlSurface());
    kLog.debug("idle inhibited");
  } else if (!want && m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
    kLog.debug("idle released");
  }
}

void PlayerApp::placeVideo() {
  AppWindow* host = videoHost();
  if (host == nullptr || !m_mpv.ready() || host->wlSurface() == nullptr) {
    return;
  }
  if (m_video == nullptr) {
    m_video = std::make_unique<VideoSurface>(loop().wayland(), loop().gl(), m_mpv);
    if (!m_video->initialize(host->wlSurface())) {
      kLog.warn("no video output");
      m_video.reset();
      return;
    }
  }
  const AppWindow::Rect r = host->contentRect();
  const std::int32_t scale = host->surface() != nullptr ? host->surface()->bufferScale() : 1;
  m_video->setGeometry(static_cast<std::int32_t>(std::lround(r.x)), static_cast<std::int32_t>(std::lround(r.y)),
                       static_cast<std::int32_t>(std::lround(r.width)), static_cast<std::int32_t>(std::lround(r.height)),
                       std::max(1, scale));
}

void PlayerApp::showControls() {
  const bool hideLater = (m_pip != nullptr || (m_window != nullptr && m_window->fullscreen())) && !m_mpv.state().paused;
  if (!m_controlsVisible) {
    m_controlsVisible = true;
    if (AppWindow* host = videoHost()) {
      host->requestRebuild();
    }
  }
  if (!hideLater) {
    m_hideControls.stop();
    return;
  }
  m_hideControls.start(kControlsTimeout, [this] {
    AppWindow* host = videoHost();
    if (host != nullptr && (host == m_pip || host->fullscreen()) && !m_mpv.state().paused) {
      m_controlsVisible = false;
      host->requestRebuild();
    }
  });
}

bool PlayerApp::libraryShown() const {
  const MpvPlayer::State& st = m_mpv.state();
  return m_library != nullptr && !st.hasVideo
      && (!m_library->tracks().empty() || !m_library->playlists().empty());
}

void PlayerApp::updateSidebar() {
  if (m_window == nullptr) {
    return;
  }
  // Video gets the whole window; music and the library get the sections.
  if (!libraryShown()) {
    m_window->setSidebarBuilder(nullptr);
    return;
  }
  m_window->setSidebarBuilder([this](Renderer&, float width, float) {
    const float s = m_window->uiScale();
    auto list = ui::column({.align = FlexAlign::Stretch, .gap = 2.0F * s, .padding = Style::spaceSm * s, .width = width});
    const auto row = [&](const char* label, const char* glyph, LibraryView view) {
      const bool current = m_view == view;
      list->addChild(ui::button({
          .text = std::string(label),
          .glyph = std::string(glyph),
          .glyphSize = 16.0F * s,
          .selected = current,
          .contentAlign = ButtonContentAlign::Start,
          .variant = current ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .radius = Style::scaledRadiusSm(s),
          .height = 32.0F * s,
          .onClick = [this, view] { showView(view); },
      }));
    };
    if (!m_mpv.state().idle) {
      row("Now Playing", "player-play", LibraryView::NowPlaying);
    }
    row("Albums", "disc", LibraryView::Albums);
    row("Artists", "microphone-2", LibraryView::Artists);
    row("Songs", "music", LibraryView::Songs);
    row("Playlists", "playlist", LibraryView::Playlists);
    return list;
  });
}

void PlayerApp::showView(LibraryView view) {
  m_view = view;
  m_openAlbum.reset();
  m_openPlaylist.reset();
  m_openArtist.clear();
  m_libraryScroll = {};
  if (m_window != nullptr) {
    m_window->requestRebuild();
  }
}

bool PlayerApp::libraryBack() {
  if (m_openAlbum.has_value()) {
    m_openAlbum.reset();
  } else if (m_openPlaylist.has_value()) {
    m_openPlaylist.reset();
  } else if (!m_openArtist.empty()) {
    m_openArtist.clear();
  } else {
    return false;
  }
  m_libraryScroll = {};
  if (m_window != nullptr) {
    m_window->requestRebuild();
  }
  return true;
}

void PlayerApp::playAlbumAt(std::size_t album, std::size_t index) {
  const auto& albums = m_library->albums();
  if (album >= albums.size()) {
    return;
  }
  std::vector<std::string> paths;
  for (const std::size_t i : albums[album].tracks) {
    paths.push_back(m_library->tracks()[i].path.string());
  }
  playQueue(paths, true, index);
}

void PlayerApp::playQueue(const std::vector<std::string>& paths, bool albumGain, std::size_t start) {
  if (paths.empty() || !m_mpv.initialize()) {
    return;
  }
  saveResume();
  m_resume = {};
  placeVideo();
  m_mpv.loadList(paths, albumGain, start);
  m_view = LibraryView::NowPlaying;
  updateSidebar();
  if (m_window != nullptr) {
    m_window->requestRebuild();
  }
}

std::unique_ptr<Node> PlayerApp::buildArt(Renderer& renderer, const std::filesystem::path& path, float size, float s) {
  // fill_1 shows while a cover decodes; an album with no cover shows the disc symbol on it.
  auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .width = size, .height = size});
  box->setFill(colorSpecFromRole(ColorRole::Fill1));
  box->setRadius(Style::scaledRadiusSm(s));
  if (path.empty()) {
    box->addChild(ui::glyph({.glyph = std::string("disc"), .glyphSize = std::round(size * 0.3F),
                             .color = colorSpecFromRole(ColorRole::Label3)}));
    return box;
  }
  auto image = ui::image({.fit = ImageFit::Cover, .radius = Style::scaledRadiusSm(s), .width = size, .height = size});
  // Decoded off the loop thread once; later rebuilds take the resident texture.
  (void)image->setSourceFileAsync(renderer, m_artCache, path.string(), static_cast<int>(size), true);
  box->addChild(std::move(image));
  return box;
}

std::unique_ptr<Node> PlayerApp::buildLibrary(Renderer& renderer, float s, float width) {
  auto scroll = ui::scrollView({.state = &m_libraryScroll, .contentScale = s, .viewportPaddingH = Style::spaceMd * s,
                                .viewportPaddingV = Style::spaceMd * s,
                                .fill = colorSpecFromRole(ColorRole::SolidRegular)});
  scroll->setFlexGrow(1.0F);
  Flex* list = scroll->content();
  list->setGap(2.0F * s);
  const auto& tracks = m_library->tracks();
  const auto& albums = m_library->albums();
  const auto heading = [&](const std::string& text, const std::string& detail) {
    list->addChild(ui::label({.text = text, .fontSize = Tokens::typeTitle2.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::End}));
    if (!detail.empty()) {
      list->addChild(ui::label({.text = detail, .fontSize = Style::fontSizeBody * s,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    }
  };
  const auto row = [&](std::string text, std::function<void()> onClick) {
    list->addChild(ui::button({
        .text = std::move(text),
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .height = 36.0F * s,
        .onClick = std::move(onClick),
    }));
  };
  const auto songs = [](std::size_t n) { return std::format("{} {}", n, n == 1 ? "song" : "songs"); };
  const auto trackRow = [&](const MusicTrack& t, std::size_t number, std::function<void()> onClick) {
    row(std::format("{:>2}   {}   {}", number, t.title, clock(t.duration)), std::move(onClick));
  };
  // Albums as a grid of square covers, title and artist under each (direction A, .agent/DESIGN.md).
  const auto albumRows = [&](const std::string& artist) {
    const float tile = kAlbumTile * s;
    const float gap = Style::spaceMd * s;
    const float inner = std::max(tile, width - 2.0F * Style::spaceMd * s);
    const auto columns = std::max<std::size_t>(1, static_cast<std::size_t>((inner + gap) / (tile + gap)));
    std::unique_ptr<Flex> line;
    std::size_t inLine = 0;
    const auto flush = [&] {
      if (line != nullptr) {
        list->addChild(std::move(line));
      }
      inLine = 0;
    };
    for (std::size_t a = 0; a < albums.size(); ++a) {
      const MusicAlbum& album = albums[a];
      if (!artist.empty() && album.artist != artist) {
        continue;
      }
      if (line == nullptr || inLine == columns) {
        flush();
        line = ui::row({.align = FlexAlign::Start, .gap = gap});
      }
      auto cell = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * s, .width = tile});
      cell->addChild(buildArt(renderer, album.art, tile, s));
      auto title = ui::label({.text = album.title, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .width = tile, .maxLines = 1,
                              .ellipsize = TextEllipsize::End});
      auto artistLabel = ui::label({.text = album.artist, .fontSize = Style::fontSizeCaption * s,
                                    .color = colorSpecFromRole(ColorRole::Label2), .width = tile, .maxLines = 1,
                                    .ellipsize = TextEllipsize::End});
      title->measure(renderer);
      artistLabel->measure(renderer);
      const float cellHeight = tile + 2.0F * Style::spaceXs * s + title->height() + artistLabel->height();
      cell->addChild(std::move(title));
      cell->addChild(std::move(artistLabel));
      // An InputArea takes no size from its child: the row lays it out by this size.
      auto press = ui::inputArea({});
      press->setSize(tile, cellHeight);
      press->setOnPress([this, a](const InputArea::PointerData& data) {
        if (data.pressed && data.button == BTN_LEFT) {
          m_openAlbum = a;
          m_libraryScroll = {};
          m_window->requestRebuild();
        }
      });
      press->addChild(std::move(cell));
      line->addChild(std::move(press));
      ++inLine;
    }
    flush();
    list->setGap(Style::spaceMd * s);
  };

  if (m_openAlbum.has_value() && *m_openAlbum < albums.size()) {
    const MusicAlbum& album = albums[*m_openAlbum];
    auto header = ui::row({.align = FlexAlign::End, .gap = Style::spaceLg * s});
    header->addChild(buildArt(renderer, album.art, 200.0F * s, s));
    auto info = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * s, .flexGrow = 1.0F});
    info->addChild(ui::label({.text = album.title, .fontSize = Tokens::typeTitle2.size * s, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2, .ellipsize = TextEllipsize::End}));
    info->addChild(ui::label({.text = std::format("{}{}{} · {}", album.artist, album.year.empty() ? "" : " · ", album.year,
                                                  songs(album.tracks.size())),
                              .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2)}));
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s});
    actions->addChild(ui::button({.text = std::string("Play"), .glyph = std::string("player-play"),
                                  .variant = ButtonVariant::Primary,
                                  .onClick = [this, a = *m_openAlbum] { playAlbumAt(a, 0); }}));
    actions->addChild(ui::button({.text = std::string("Shuffle"), .glyph = std::string("arrows-shuffle"),
                                  .onClick = [this, a = *m_openAlbum] { playAlbumShuffled(a); }}));
    info->addChild(std::move(actions));
    header->addChild(std::move(info));
    list->addChild(std::move(header));
    list->addChild(ui::node({.height = Style::spaceMd * s}));
    for (std::size_t i = 0; i < album.tracks.size(); ++i) {
      const MusicTrack& t = tracks[album.tracks[i]];
      trackRow(t, t.track > 0 ? static_cast<std::size_t>(t.track) : i + 1, [this, a = *m_openAlbum, i] { playAlbumAt(a, i); });
    }
    return scroll;
  }
  if (m_openPlaylist.has_value() && *m_openPlaylist < m_library->playlists().size()) {
    const MusicPlaylist& playlist = m_library->playlists()[*m_openPlaylist];
    heading(playlist.name, songs(playlist.entries.size()));
    for (std::size_t i = 0; i < playlist.entries.size(); ++i) {
      const auto& entry = playlist.entries[i];
      const auto it = std::ranges::find_if(tracks, [&](const MusicTrack& t) { return t.path == entry; });
      MusicTrack shown = it != tracks.end() ? *it : MusicTrack{.path = entry, .title = entry.filename().string()};
      trackRow(shown, i + 1, [this, name = playlist.name, i] { playPlaylist(name, i); });
    }
    return scroll;
  }
  switch (m_view) {
  case LibraryView::NowPlaying:
  case LibraryView::Albums:
    heading("Albums", {});
    albumRows({});
    break;
  case LibraryView::Artists:
    if (!m_openArtist.empty()) {
      heading(m_openArtist, {});
      albumRows(m_openArtist);
      break;
    }
    heading("Artists", {});
    {
      std::vector<std::string> artists;
      for (const MusicAlbum& album : albums) {
        if (!std::ranges::contains(artists, album.artist)) {
          artists.push_back(album.artist);
        }
      }
      std::ranges::sort(artists);
      for (const std::string& artist : artists) {
        row(artist, [this, artist] {
          m_openArtist = artist;
          m_libraryScroll = {};
          m_window->requestRebuild();
        });
      }
    }
    break;
  case LibraryView::Songs: {
    heading("Songs", songs(tracks.size()));
    // Songs in library order (artist, album, disc, track): a click plays from there to the end.
    for (std::size_t a = 0, n = 0; a < albums.size(); ++a) {
      for (const std::size_t i : albums[a].tracks) {
        const MusicTrack& t = tracks[i];
        row(std::format("{}  ·  {} — {}", t.title, t.artist, t.album), [this, n] { playSongsAt(n); });
        ++n;
      }
    }
    break;
  }
  case LibraryView::Playlists:
    heading("Playlists", {});
    for (std::size_t p = 0; p < m_library->playlists().size(); ++p) {
      const MusicPlaylist& playlist = m_library->playlists()[p];
      row(std::format("{}  ·  {}", playlist.name, songs(playlist.entries.size())), [this, p] {
        m_openPlaylist = p;
        m_libraryScroll = {};
        m_window->requestRebuild();
      });
    }
    break;
  }
  return scroll;
}

void PlayerApp::playAlbumShuffled(std::size_t album) {
  const auto& albums = m_library->albums();
  if (album >= albums.size()) {
    return;
  }
  std::vector<std::string> paths;
  for (const std::size_t i : albums[album].tracks) {
    paths.push_back(m_library->tracks()[i].path.string());
  }
  std::shuffle(paths.begin(), paths.end(), std::mt19937{std::random_device{}()});
  playQueue(paths, true, 0);
}

void PlayerApp::playSongsAt(std::size_t index) {
  std::vector<std::string> paths;
  for (const MusicAlbum& album : m_library->albums()) {
    for (const std::size_t i : album.tracks) {
      paths.push_back(m_library->tracks()[i].path.string());
    }
  }
  playQueue(paths, false, index);
}

std::unique_ptr<Node> PlayerApp::buildNowPlaying(Renderer& renderer, float s) {
  const MpvPlayer::State& st = m_mpv.state();
  std::string title = st.title;
  std::string artist;
  std::string album;
  std::filesystem::path art;
  if (m_library != nullptr) {
    const auto& tracks = m_library->tracks();
    const auto it = std::ranges::find_if(tracks, [&](const MusicTrack& t) { return t.path == st.path; });
    if (it != tracks.end()) {
      // mpv's media-title arrives after the path: the tag title is known already.
      title = !it->title.empty() ? it->title : title;
      artist = it->artist;
      album = it->album;
      // By track index: a compilation's album artist is "Various Artists", not the track's artist.
      const auto index = static_cast<std::size_t>(it - tracks.begin());
      for (const MusicAlbum& a : m_library->albums()) {
        if (std::ranges::contains(a.tracks, index)) {
          art = a.art;
          break;
        }
      }
    }
  }
  std::error_code localError;
  // A local file outside the library: its cover is looked up once per file (headers only). A URL never is: an
  // http read has no timeout and this runs on the loop thread.
  if (art.empty() && st.path.starts_with('/') && std::filesystem::is_regular_file(st.path, localError)) {
    if (m_looseArtPath != st.path) {
      m_looseArtPath = st.path;
      m_looseArt.clear();
      MusicTrack track;
      if (MusicLibrary::readTags(st.path, track)) {
        MusicAlbum single{.title = track.album, .artist = track.artist, .tracks = {0}};
        const std::vector<MusicTrack> one{track};
        MusicLibrary::findArt(single, one, std::filesystem::path(g_get_user_cache_dir()) / "andromeda/player/art");
        m_looseArt = single.art;
      }
    }
    art = m_looseArt;
  }
  auto panel = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                           .flexGrow = 1.0F});
  panel->setFill(colorSpecFromRole(ColorRole::SolidRegular));
  panel->addChild(buildArt(renderer, art, 280.0F * s, s));
  panel->addChild(ui::label({.text = title, .fontSize = Tokens::typeTitle2.size * s, .fontWeight = FontWeight::SemiBold,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::End}));
  if (!artist.empty() || !album.empty()) {
    panel->addChild(ui::label({.text = artist + (artist.empty() || album.empty() ? "" : " — ") + album,
                               .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label2),
                               .maxLines = 1, .ellipsize = TextEllipsize::End}));
  }
  return panel;
}

std::unique_ptr<Node> PlayerApp::buildContent(Renderer& renderer, float width, float height) {
  const float s = m_window->uiScale();
  const MpvPlayer::State& st = m_mpv.state();
  m_time = m_remaining = nullptr;
  m_seek = nullptr;
  m_shownSecond = -1;
  auto content = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});

  // The video area: click toggles pause, double-click toggles full screen, motion shows the controls.
  auto area = ui::inputArea({});
  area->setFlexGrow(1.0F);
  area->setOnPress([this, last = std::uint32_t{0}](const InputArea::PointerData& data) mutable {
    if (!data.pressed || data.button != BTN_LEFT) {
      return;
    }
    if (last != 0 && data.time - last < kDoubleClickMs) {
      last = 0;
      m_mpv.togglePause(); // undo the first click's toggle
      if (m_window != nullptr) {
        m_window->toggleFullscreen();
      }
      return;
    }
    last = data.time;
    m_mpv.togglePause();
  });
  area->setOnMotion([this](const InputArea::PointerData&) { showControls(); });
  // An InputArea does not stretch its child: the pane takes the area's size, and its child grows into it.
  const bool barShown = !st.idle && m_controlsVisible;
  auto pane = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height - (barShown ? kBarHeight * s : 0.0F)});
  if (m_pip != nullptr) {
    auto away = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                            .flexGrow = 1.0F});
    away->setFill(colorSpecFromRole(ColorRole::SolidRegular));
    away->addChild(ui::glyph({.glyph = std::string("picture-in-picture"), .glyphSize = 48.0F * s,
                              .color = colorSpecFromRole(ColorRole::Label3)}));
    away->addChild(ui::label({.text = std::string("Playing in Picture in Picture"), .fontSize = Style::fontSizeBody * s,
                              .color = colorSpecFromRole(ColorRole::Label2)}));
    pane->addChild(std::move(away));
  } else if (libraryShown() && !(m_view == LibraryView::NowPlaying && !st.idle)) {
    pane->addChild(buildLibrary(renderer, s, width));
  } else if (!st.idle && !st.hasVideo) {
    pane->addChild(buildNowPlaying(renderer, s));
  } else if (st.idle) {
    auto empty = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                             .flexGrow = 1.0F});
    empty->setFill(colorSpecFromRole(ColorRole::SolidRegular));
    empty->addChild(ui::glyph({.glyph = std::string("movie"), .glyphSize = 48.0F * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
    empty->addChild(ui::label({.text = std::string("No video"),
                               .fontSize = Tokens::typeTitle3.size * s, .color = colorSpecFromRole(ColorRole::Label1)}));
    empty->addChild(ui::label({.text = std::string("Open a video from Files"), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
    pane->addChild(std::move(empty));
  }
  area->addChild(std::move(pane));
  content->addChild(std::move(area));
  if (st.idle || !m_controlsVisible) {
    return content;
  }

  auto bar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                      .height = kBarHeight * s});
  bar->setFill(colorSpecFromRole(ColorRole::SolidRegular));
  const bool queue = st.queueCount > 1;
  if (queue) {
    bar->addChild(ui::button({.glyph = std::string("player-skip-back"), .glyphSize = 18.0F * s,
                              .variant = ButtonVariant::Ghost, .onClick = [this] { m_mpv.previous(); }}));
  }
  bar->addChild(ui::button({
      .glyph = std::string(st.paused || st.eof ? "player-play" : "player-pause"),
      .glyphSize = 20.0F * s,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { m_mpv.togglePause(); },
  }));
  if (queue) {
    bar->addChild(ui::button({.glyph = std::string("player-skip-forward"), .glyphSize = 18.0F * s,
                              .enabled = st.queuePos + 1 < st.queueCount, .variant = ButtonVariant::Ghost,
                              .onClick = [this] { m_mpv.next(); }}));
  }
  bar->addChild(ui::label({.out = &m_time, .text = clock(st.position), .fontSize = Style::fontSizeCaption * s,
                           .color = colorSpecFromRole(ColorRole::Label2), .width = 56.0F * s, .textAlign = TextAlign::End}));
  bar->addChild(ui::slider({
      .out = &m_seek,
      .minValue = 0.0,
      .maxValue = 1.0,
      .value = st.duration > 0.0 ? st.position / st.duration : 0.0,
      .enabled = st.duration > 0.0,
      .flexGrow = 1.0F,
      .onDragEnd = [this] {
        if (m_seek != nullptr) {
          m_mpv.seekTo(m_seek->value() * m_mpv.state().duration);
        }
      },
  }));
  bar->addChild(ui::label({.out = &m_remaining, .text = "-" + clock(st.duration - st.position),
                           .fontSize = Style::fontSizeCaption * s, .color = colorSpecFromRole(ColorRole::Label2),
                           .width = 56.0F * s}));
  if (st.subtitleTracks > 0) {
    bar->addChild(ui::button({
        .text = st.subtitle.empty() ? std::string("Off") : st.subtitle,
        .glyph = std::string("badge-cc"),
        .glyphSize = 18.0F * s,
        .variant = ButtonVariant::Ghost,
        .onClick = [this] { m_mpv.cycleSubtitles(); },
    }));
  }
  bar->addChild(ui::button({
      .glyph = std::string(m_pip != nullptr ? "picture-in-picture-off" : "picture-in-picture"),
      .glyphSize = 18.0F * s,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { DeferredCall::callLater([this] { togglePip(); }); },
  }));
  bar->addChild(ui::button({
      .glyph = std::string(m_window->fullscreen() ? "minimize" : "maximize"),
      .glyphSize = 18.0F * s,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { m_window->toggleFullscreen(); },
  }));
  content->addChild(std::move(bar));
  return content;
}
