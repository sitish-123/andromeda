#include "gallery_video.h"

#include "app/app_loop.h"
#include "audio_player.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "helper_client.h"
#include "video_view.h"

#include <algorithm>
#include <fcntl.h>
#include <format>
#include <unistd.h>

namespace {
  constexpr Logger kLog("gallery-video");
  constexpr std::uint32_t kFlagVideo = 1;
  constexpr std::uint32_t kFlagAudio = 2;
} // namespace

GalleryVideo::GalleryVideo(AppLoop& loop, WaylandConnection& wayland, Callbacks callbacks)
    : m_loop(loop), m_wayland(wayland), m_callbacks(std::move(callbacks)) {}

GalleryVideo::~GalleryVideo() { stop(); }

void GalleryVideo::stop() {
  ++m_generation;
  m_tick.stop();
  m_video.reset(); // before the helper: the view returns its frames to it
  m_audio.reset();
  if (m_helper) {
    m_loop.removeSource(m_helper.get());
    m_helper.reset();
  }
  m_durationMs = 0;
}

void GalleryVideo::play(const std::filesystem::path& video, bool sound, std::array<float, 4> rect) {
  m_rect = rect;
  start(video, sound, false);
}

void GalleryVideo::start(const std::filesystem::path& video, bool sound, bool software) {
  stop();
  m_helper = preview::HelperClient::start(software ? preview::HelperKind::MediaSoftware : preview::HelperKind::Media,
                                          video.string());
  if (!m_helper) {
    kLog.warn("no media helper for {}", video.string());
    return;
  }
  m_loop.addSource(m_helper.get());
  const auto generation = m_generation;
  m_helper->request({.op = preview::op::Info}, [this, generation, sound, software, video, name = video.filename().string()](
                                                   const preview::HelperReply& reply) {
    if (generation != m_generation) {
      return;
    }
    if (reply.msg.op != preview::op::InfoReply || (reply.msg.e & kFlagVideo) == 0) {
      kLog.warn("{}: no video stream", name);
      DeferredCall::callLater([this, generation] {
        if (generation == m_generation) {
          stop();
          if (m_callbacks.ended) {
            m_callbacks.ended();
          }
        }
      });
      return;
    }
    m_durationMs = reply.msg.d;
    m_video = std::make_unique<preview::VideoView>(
        m_wayland, *m_helper,
        preview::VideoView::Callbacks{
            .firstFrame = [name](std::uint32_t w, std::uint32_t h) { kLog.debug("gallery video {} {}x{}", name, w, h); },
            .failed = [this, generation, video, sound, software, name](bool unsupported) {
              kLog.warn("{}: video failed{}", name, unsupported && !software ? ", trying software frames" : "");
              // Runs inside the helper's callback: restart or stop after it returns.
              DeferredCall::callLater([this, generation, video, sound, software, unsupported] {
                if (generation != m_generation) {
                  return;
                }
                if (unsupported && !software) {
                  start(video, sound, true);
                  return;
                }
                stop();
                if (m_callbacks.ended) {
                  m_callbacks.ended();
                }
              });
            },
            .moved = [this] {
              if (m_callbacks.redraw) {
                m_callbacks.redraw();
              }
            },
        });
    m_video->setVideoSize(reply.msg.b, reply.msg.c);
    // Audio first: the helper starts on the first request, and audio decoded before OP_AUDIO is dropped.
    if (sound && (reply.msg.e & kFlagAudio) != 0) {
      int fds[2];
      if (::pipe2(fds, O_CLOEXEC) == 0) {
        ::fcntl(fds[1], F_SETPIPE_SZ, 1 << 20); // about 1.4 s of PCM in flight: a full pipe is the backpressure
        m_audio = preview::AudioPlayer::start(fds[0]);
        if (m_audio) {
          m_helper->request({.op = preview::op::Audio}, fds[1], [](const preview::HelperReply&) {});
          m_video->setClock([this]() { return m_audio ? m_audio->position() : std::nullopt; });
        }
        ::close(fds[1]);
      }
    }
    wl_surface* parent = m_callbacks.parent ? m_callbacks.parent() : nullptr;
    if (parent == nullptr || !m_video->attach(parent, true)) {
      stop();
      return;
    }
    m_video->place(m_rect[0], m_rect[1], m_rect[2], m_rect[3]);
    m_tick.startRepeating(std::chrono::milliseconds(250), [this] { tick(); });
    kLog.debug("gallery video playing {}{}", name, m_audio ? " with sound" : " muted");
  });
}

void GalleryVideo::place(std::array<float, 4> rect) {
  if (rect == m_rect) {
    return;
  }
  m_rect = rect;
  if (m_video) {
    m_video->place(rect[0], rect[1], rect[2], rect[3]);
  }
}

void GalleryVideo::toggle() {
  if (!m_video) {
    return;
  }
  m_video->togglePlaying();
  if (m_audio) {
    m_audio->setPlaying(m_video->playing());
  }
  tick();
}

void GalleryVideo::tick() {
  if (!m_video) {
    return;
  }
  if (m_video->finished()) {
    kLog.debug("gallery video ended");
    stop();
    if (m_callbacks.ended) {
      m_callbacks.ended();
    }
    return;
  }
  const auto position = m_audio ? m_audio->position().value_or(m_video->shownPts()) : m_video->shownPts();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(position).count();
  const auto clock = [](std::int64_t t) { return std::format("{}:{:02}", t / 60000, (t / 1000) % 60); };
  const float progress = m_durationMs > 0 ? std::clamp(static_cast<float>(ms) / static_cast<float>(m_durationMs), 0.0F, 1.0F) : 0.0F;
  if (m_callbacks.progress) {
    m_callbacks.progress(m_video->playing(), progress,
                         m_durationMs > 0 ? clock(ms) + " / " + clock(m_durationMs) : clock(ms));
  }
}
