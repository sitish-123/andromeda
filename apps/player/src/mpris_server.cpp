#include "mpris_server.h"

#include "dbus/session_bus.h"
#include "mpv_player.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glib.h>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <vector>

namespace {
  const sdbus::ServiceName kBusName{"org.mpris.MediaPlayer2.andromeda.Player"};
  const sdbus::ObjectPath kPath{"/org/mpris/MediaPlayer2"};
  constexpr auto kRoot = "org.mpris.MediaPlayer2";
  constexpr auto kPlayer = "org.mpris.MediaPlayer2.Player";
  const sdbus::ObjectPath kNoTrack{"/org/mpris/MediaPlayer2/TrackList/NoTrack"};

  std::int64_t micros(double seconds) { return static_cast<std::int64_t>(std::llround(seconds * 1e6)); }

  std::string uriFor(const std::string& path) {
    if (path.find("://") != std::string::npos) {
      return path;
    }
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string out = uri != nullptr ? uri : path;
    g_free(uri);
    return out;
  }
} // namespace

MprisServer::MprisServer(SessionBus& bus, MpvPlayer& player, Actions actions)
    : m_player(player), m_actions(std::move(actions)) {
  m_object = sdbus::createObject(bus.connection(), kPath);
  const auto loaded = [this] { return !m_player.state().idle; };
  m_object
      ->addVTable(
          sdbus::registerMethod("Raise").implementedAs([this] {
            if (m_actions.raise) {
              m_actions.raise();
            }
          }),
          sdbus::registerMethod("Quit").implementedAs([this] {
            if (m_actions.quit) {
              m_actions.quit();
            }
          }),
          sdbus::registerProperty("CanQuit").withGetter([] { return true; }),
          sdbus::registerProperty("CanRaise").withGetter([] { return true; }),
          sdbus::registerProperty("HasTrackList").withGetter([] { return false; }),
          sdbus::registerProperty("Identity").withGetter([] { return std::string("Player"); }),
          sdbus::registerProperty("DesktopEntry").withGetter([] { return std::string("dev.andromeda.Player"); }),
          sdbus::registerProperty("SupportedUriSchemes").withGetter([] {
            return std::vector<std::string>{"file", "http", "https"};
          }),
          sdbus::registerProperty("SupportedMimeTypes").withGetter([] {
            return std::vector<std::string>{"video/*", "audio/*"};
          })
      )
      .forInterface(kRoot);
  m_object
      ->addVTable(
          sdbus::registerMethod("Play").implementedAs([this] { m_player.setPaused(false); }),
          sdbus::registerMethod("Pause").implementedAs([this] { m_player.setPaused(true); }),
          sdbus::registerMethod("PlayPause").implementedAs([this] { m_player.togglePause(); }),
          sdbus::registerMethod("Stop").implementedAs([this] { m_player.stop(); }),
          sdbus::registerMethod("Next").implementedAs([] {}),
          sdbus::registerMethod("Previous").implementedAs([] {}),
          sdbus::registerMethod("Seek").withInputParamNames("Offset").implementedAs([this](std::int64_t offset) {
            m_player.seekBy(static_cast<double>(offset) / 1e6);
          }),
          sdbus::registerMethod("SetPosition")
              .withInputParamNames("TrackId", "Position")
              .implementedAs([this](const sdbus::ObjectPath& track, std::int64_t position) {
                // The spec ignores a stale track id or a position outside the track.
                const double seconds = static_cast<double>(position) / 1e6;
                if (track == trackId() && seconds >= 0.0 && seconds <= m_player.state().duration) {
                  m_player.seekTo(seconds);
                }
              }),
          sdbus::registerMethod("OpenUri").withInputParamNames("Uri").implementedAs([this](const std::string& uri) {
            if (m_actions.openUri) {
              m_actions.openUri(uri);
            }
          }),
          sdbus::registerSignal("Seeked").withParameters<std::int64_t>("Position"),
          sdbus::registerProperty("PlaybackStatus").withGetter([this] { return status(); }),
          sdbus::registerProperty("Rate").withGetter([] { return 1.0; }),
          sdbus::registerProperty("MinimumRate").withGetter([] { return 1.0; }),
          sdbus::registerProperty("MaximumRate").withGetter([] { return 1.0; }),
          sdbus::registerProperty("Metadata").withGetter([this] {
            const MpvPlayer::State& s = m_player.state();
            std::map<std::string, sdbus::Variant> meta;
            meta["mpris:trackid"] = sdbus::Variant(s.idle ? kNoTrack : trackId());
            if (!s.idle) {
              meta["mpris:length"] = sdbus::Variant(micros(s.duration));
              meta["xesam:title"] = sdbus::Variant(s.title);
              meta["xesam:url"] = sdbus::Variant(uriFor(s.path));
            }
            return meta;
          }),
          sdbus::registerProperty("Volume")
              .withGetter([this] { return m_player.state().volume / 100.0; })
              .withSetter([this](double volume) { m_player.setVolume(std::clamp(volume, 0.0, 1.3) * 100.0); }), // mpv volume-max 130
          sdbus::registerProperty("Position").withGetter([this] { return micros(m_player.state().position); }),
          sdbus::registerProperty("CanGoNext").withGetter([] { return false; }),
          sdbus::registerProperty("CanGoPrevious").withGetter([] { return false; }),
          sdbus::registerProperty("CanPlay").withGetter(loaded),
          sdbus::registerProperty("CanPause").withGetter(loaded),
          sdbus::registerProperty("CanSeek").withGetter([this] { return m_player.state().duration > 0.0; }),
          sdbus::registerProperty("CanControl").withGetter([] { return true; })
      )
      .forInterface(kPlayer);
  bus.connection().requestName(kBusName);
}

MprisServer::~MprisServer() = default;

sdbus::ObjectPath MprisServer::trackId() const {
  return sdbus::ObjectPath{std::format("/dev/andromeda/Player/track/{}", m_track)};
}

std::string MprisServer::status() const {
  const MpvPlayer::State& s = m_player.state();
  if (s.idle) {
    return "Stopped";
  }
  return s.paused || s.eof ? "Paused" : "Playing";
}

void MprisServer::update(bool seeked) {
  const MpvPlayer::State& s = m_player.state();
  std::vector<sdbus::PropertyName> changed;
  if (const std::string now = status(); now != m_lastStatus) {
    m_lastStatus = now;
    changed.emplace_back("PlaybackStatus");
    changed.emplace_back("CanPlay");
    changed.emplace_back("CanPause");
  }
  if (s.path != m_lastPath || s.title != m_lastTitle || s.duration != m_lastDuration) {
    if (s.path != m_lastPath) {
      ++m_track; // a new track id per loaded file (spec: unique per track)
    }
    m_lastPath = s.path;
    m_lastTitle = s.title;
    m_lastDuration = s.duration;
    changed.emplace_back("Metadata");
    changed.emplace_back("CanSeek");
  }
  if (s.volume != m_lastVolume) {
    m_lastVolume = s.volume;
    changed.emplace_back("Volume");
  }
  // Position is not signalled on its own: clients read it and follow Rate (spec).
  if (!changed.empty()) {
    m_object->emitPropertiesChangedSignal(sdbus::InterfaceName{kPlayer}, changed);
  }
  if (seeked) {
    m_object->emitSignal("Seeked").onInterface(kPlayer).withArguments(micros(s.position));
  }
}
