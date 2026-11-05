#include "mpv_player.h"

#include "core/log.h"
#include "mpv_lib.h"

#include <format>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace {
  constexpr Logger kLog("mpv");

  enum Observed : std::uint64_t { kPath = 1, kTitle, kDuration, kPosition, kPause, kIdle, kVoConfigured, kAlbumArt, kWidth, kHeight, kVolume, kMute, kEof };
} // namespace

MpvPlayer::MpvPlayer() = default;

MpvPlayer::~MpvPlayer() {
  if (m_mpv != nullptr) {
    m_lib->setWakeupCallback(m_mpv, nullptr, nullptr);
    m_lib->terminateDestroy(m_mpv);
  }
  if (m_wakeFd >= 0) {
    ::close(m_wakeFd);
  }
}

bool MpvPlayer::initialize() {
  if (m_mpv != nullptr) {
    return true;
  }
  m_lib = MpvLib::get();
  if (m_lib == nullptr) {
    return false;
  }
  m_mpv = m_lib->create();
  if (m_mpv == nullptr) {
    return false;
  }
  const std::pair<const char*, const char*> options[] = {
      {"vo", "libmpv"},
      {"hwdec", "auto-safe"},
      {"keep-open", "yes"}, // the last frame stays at the end; eof-reached reports it
      {"input-default-bindings", "no"},
      {"input-vo-keyboard", "no"},
      {"osc", "no"},
      {"terminal", "no"},
      {"audio-client-name", "andromeda-player"},
      {"msg-level", "all=warn"},
  };
  for (const auto& [name, value] : options) {
    if (const int err = m_lib->setOptionString(m_mpv, name, value); err < 0) {
      kLog.warn("mpv option {}={}: {}", name, value, m_lib->errorString(err));
    }
  }
  if (const int err = m_lib->initialize(m_mpv); err < 0) {
    kLog.warn("mpv_initialize: {}", m_lib->errorString(err));
    m_lib->terminateDestroy(m_mpv);
    m_mpv = nullptr;
    return false;
  }
  const std::pair<std::uint64_t, std::pair<const char*, mpv_format>> props[] = {
      {kPath, {"path", MPV_FORMAT_STRING}},          {kTitle, {"media-title", MPV_FORMAT_STRING}},
      {kDuration, {"duration", MPV_FORMAT_DOUBLE}},  {kPosition, {"time-pos", MPV_FORMAT_DOUBLE}},
      {kPause, {"pause", MPV_FORMAT_FLAG}},          {kIdle, {"idle-active", MPV_FORMAT_FLAG}},
      {kWidth, {"width", MPV_FORMAT_INT64}},           {kVoConfigured, {"vo-configured", MPV_FORMAT_FLAG}},
      {kAlbumArt, {"current-tracks/video/albumart", MPV_FORMAT_FLAG}},
      {kHeight, {"height", MPV_FORMAT_INT64}},       {kVolume, {"volume", MPV_FORMAT_DOUBLE}},
      {kMute, {"mute", MPV_FORMAT_FLAG}},            {kEof, {"eof-reached", MPV_FORMAT_FLAG}},
  };
  for (const auto& [id, prop] : props) {
    m_lib->observeProperty(m_mpv, id, prop.first, prop.second);
  }
  m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  // Runs on mpv threads: only the eventfd write is safe here.
  m_lib->setWakeupCallback(
      m_mpv,
      [](void* data) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto n = ::write(*static_cast<int*>(data), &one, sizeof one);
      },
      &m_wakeFd
  );
  return true;
}

void MpvPlayer::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_wakeFd >= 0) {
    fds.push_back({.fd = m_wakeFd, .events = POLLIN, .revents = 0});
  }
}

void MpvPlayer::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  if (m_mpv == nullptr) {
    return;
  }
  std::uint64_t count = 0;
  [[maybe_unused]] const auto n = ::read(m_wakeFd, &count, sizeof count);
  bool changed = false;
  bool seeked = false;
  for (;;) {
    const mpv_event* event = m_lib->waitEvent(m_mpv, 0);
    if (event->event_id == MPV_EVENT_NONE) {
      break;
    }
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE:
      onProperty(event->reply_userdata, *static_cast<const mpv_event_property*>(event->data));
      changed = true;
      break;
    case MPV_EVENT_PLAYBACK_RESTART:
      // A file start is a restart too; only a requested seek is an MPRIS Seeked.
      if (m_seekPending) {
        m_seekPending = false;
        seeked = true;
        double pos = 0.0;
        if (m_lib->getProperty(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) {
          m_state.position = pos;
        }
      }
      changed = true;
      break;
    case MPV_EVENT_END_FILE:
      if (const auto* end = static_cast<const mpv_event_end_file*>(event->data); end->reason == MPV_END_FILE_REASON_ERROR) {
        kLog.warn("playback failed: {}", m_lib->errorString(end->error));
      }
      changed = true;
      break;
    default:
      break;
    }
  }
  if (changed && m_onChanged) {
    m_onChanged(seeked);
  }
}

void MpvPlayer::onProperty(std::uint64_t id, const mpv_event_property& p) {
  const bool has = p.format != MPV_FORMAT_NONE && p.data != nullptr;
  const auto str = [&] { return has ? std::string(*static_cast<char**>(p.data)) : std::string(); };
  const auto dbl = [&] { return has ? *static_cast<double*>(p.data) : 0.0; };
  const auto flag = [&] { return has && *static_cast<int*>(p.data) != 0; };
  const auto i64 = [&] { return has ? *static_cast<std::int64_t*>(p.data) : std::int64_t{0}; };
  switch (id) {
  case kPath: m_state.path = str(); break;
  case kTitle: m_state.title = str(); break;
  case kDuration: m_state.duration = dbl(); break;
  case kPosition: m_state.position = dbl(); break;
  case kPause: m_state.paused = flag(); break;
  case kIdle: m_state.idle = flag(); break;
  case kWidth:
    m_state.width = i64();
    break;
  case kVoConfigured:
    m_voConfigured = flag();
    break;
  case kAlbumArt:
    m_albumArt = flag();
    break;
  case kHeight: m_state.height = i64(); break;
  case kVolume: m_state.volume = dbl(); break;
  case kMute: m_state.muted = flag(); break;
  case kEof: m_state.eof = flag(); break;
  default: break;
  }
  // A configured output that is not a cover picture: a video track plays.
  m_state.hasVideo = m_voConfigured && !m_albumArt;
}

void MpvPlayer::command(std::initializer_list<const char*> args) {
  std::vector<const char*> argv(args);
  argv.push_back(nullptr);
  if (const int err = m_lib->commandAsync(m_mpv, 0, argv.data()); err < 0) {
    kLog.warn("mpv {}: {}", argv.front(), m_lib->errorString(err));
  }
}

void MpvPlayer::load(const std::string& path, double startSeconds) {
  if (!initialize()) {
    return;
  }
  m_state.eof = false;
  const std::string options = startSeconds > 0.0 ? std::format("start={:.3f}", startSeconds) : std::string();
  // mpv 0.38 (client API 2.3) added <index> before <options>; older cores read "-1" as the options.
  if (m_lib->clientApiVersion() >= MPV_MAKE_VERSION(2, 3)) {
    command({"loadfile", path.c_str(), "replace", "-1", options.c_str()});
  } else {
    command({"loadfile", path.c_str(), "replace", options.c_str()});
  }
  setPaused(false);
}

void MpvPlayer::setPaused(bool paused) {
  if (m_mpv == nullptr) {
    return;
  }
  int flag = paused ? 1 : 0;
  m_lib->setProperty(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::seekTo(double seconds) {
  if (m_mpv != nullptr) {
    const std::string value = std::format("{:.3f}", seconds);
    m_seekPending = true;
    command({"seek", value.c_str(), "absolute+exact"});
  }
}

void MpvPlayer::seekBy(double seconds) {
  if (m_mpv != nullptr) {
    const std::string value = std::format("{:.3f}", seconds);
    m_seekPending = true;
    command({"seek", value.c_str(), "relative"});
  }
}

void MpvPlayer::setVolume(double volume) {
  if (m_mpv != nullptr) {
    m_lib->setProperty(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
  }
}

void MpvPlayer::stop() {
  if (m_mpv != nullptr) {
    command({"stop"});
  }
}
