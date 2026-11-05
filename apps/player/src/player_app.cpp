#include "player_app.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "mpris_server.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "video_surface.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <cmath>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("player");
  constexpr float kBarHeight = 56.0F;
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
}

void PlayerApp::startup() {
  loop().addSource(&m_mpv);
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
      .transparentContent = true,
      .headerWhenFullscreen = false,
  });
  m_window = &window;
  window.setContentBuilder([this](Renderer& /*renderer*/, float width, float height) { return buildContent(width, height); });
  window.setLayoutCallback([this] { placeVideo(); });
  return window;
}

void PlayerApp::windowClosed(AppWindow& window) {
  if (&window != m_window) {
    return;
  }
  saveResume();
  m_video.reset();
  if (m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
  }
  m_window = nullptr;
  m_time = m_remaining = nullptr;
  m_seek = nullptr;
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
  const bool want = m_window != nullptr && s.hasVideo && !s.paused && !s.idle && !s.eof;
  if (const int inputs = (m_window != nullptr) | s.hasVideo << 1 | s.paused << 2 | s.idle << 3 | s.eof << 4;
      inputs != m_inhibitInputs) {
    m_inhibitInputs = inputs;
    kLog.debug("inhibit want={} video={} paused={} idle={} eof={} manager={}", want, s.hasVideo, s.paused, s.idle, s.eof,
               loop().wayland().idleInhibitManager() != nullptr);
  }
  zwp_idle_inhibit_manager_v1* manager = loop().wayland().idleInhibitManager();
  if (want && m_inhibitor == nullptr && manager != nullptr && m_window->wlSurface() != nullptr) {
    m_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(manager, m_window->wlSurface());
    kLog.debug("idle inhibited");
  } else if (!want && m_inhibitor != nullptr) {
    zwp_idle_inhibitor_v1_destroy(m_inhibitor);
    m_inhibitor = nullptr;
    kLog.debug("idle released");
  }
}

void PlayerApp::placeVideo() {
  if (m_window == nullptr || !m_mpv.ready() || m_window->wlSurface() == nullptr) {
    kLog.debug("video not placed: window {} mpv {}", m_window != nullptr, m_mpv.ready());
    return;
  }
  if (m_video == nullptr) {
    m_video = std::make_unique<VideoSurface>(loop().wayland(), loop().gl(), m_mpv);
    if (!m_video->initialize(m_window->wlSurface())) {
      kLog.warn("no video output");
      m_video.reset();
      return;
    }
  }
  const AppWindow::Rect r = m_window->contentRect();
  const std::int32_t scale = m_window->surface() != nullptr ? m_window->surface()->bufferScale() : 1;
  m_video->setGeometry(static_cast<std::int32_t>(std::lround(r.x)), static_cast<std::int32_t>(std::lround(r.y)),
                       static_cast<std::int32_t>(std::lround(r.width)), static_cast<std::int32_t>(std::lround(r.height)),
                       std::max(1, scale));
}

void PlayerApp::showControls() {
  const bool hideLater = m_window != nullptr && m_window->fullscreen() && !m_mpv.state().paused;
  if (!m_controlsVisible) {
    m_controlsVisible = true;
    if (m_window != nullptr) {
      m_window->requestRebuild();
    }
  }
  if (!hideLater) {
    m_hideControls.stop();
    return;
  }
  m_hideControls.start(kControlsTimeout, [this] {
    if (m_window != nullptr && m_window->fullscreen() && !m_mpv.state().paused) {
      m_controlsVisible = false;
      m_window->requestRebuild();
    }
  });
}

std::unique_ptr<Node> PlayerApp::buildContent(float width, float height) {
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
  if (st.idle) {
    auto empty = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                             .flexGrow = 1.0F});
    empty->setFill(colorSpecFromRole(ColorRole::SolidRegular));
    empty->addChild(ui::glyph({.glyph = std::string("movie"), .glyphSize = 48.0F * s,
                               .color = colorSpecFromRole(ColorRole::Label3)}));
    empty->addChild(ui::label({.text = std::string("No video"),
                               .fontSize = Tokens::typeTitle3.size * s, .color = colorSpecFromRole(ColorRole::Label1)}));
    empty->addChild(ui::label({.text = std::string("Open a video from Files"), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
    area->addChild(std::move(empty));
  }
  content->addChild(std::move(area));
  if (st.idle || !m_controlsVisible) {
    return content;
  }

  auto bar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                      .height = kBarHeight * s});
  bar->setFill(colorSpecFromRole(ColorRole::SolidRegular));
  bar->addChild(ui::button({
      .glyph = std::string(st.paused || st.eof ? "player-play" : "player-pause"),
      .glyphSize = 20.0F * s,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { m_mpv.togglePause(); },
  }));
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
  bar->addChild(ui::button({
      .glyph = std::string(m_window->fullscreen() ? "minimize" : "maximize"),
      .glyphSize = 18.0F * s,
      .variant = ButtonVariant::Ghost,
      .onClick = [this] { m_window->toggleFullscreen(); },
  }));
  content->addChild(std::move(bar));
  return content;
}
