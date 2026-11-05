#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class MpvPlayer;
class SessionBus;

namespace sdbus {
  class IObject;
  class ObjectPath;
}

// org.mpris.MediaPlayer2 + .Player for the player, on org.mpris.MediaPlayer2.andromeda.Player, so the bar's media
// widget and Link control it with no extra code.
class MprisServer {
public:
  struct Actions {
    std::function<void()> raise;
    std::function<void()> quit;
    std::function<void(const std::string& uri)> openUri;
  };

  MprisServer(SessionBus& bus, MpvPlayer& player, Actions actions);
  ~MprisServer();

  MprisServer(const MprisServer&) = delete;
  MprisServer& operator=(const MprisServer&) = delete;

  // After a player change: emits PropertiesChanged for what changed, and Seeked after a seek.
  void update(bool seeked);

private:
  [[nodiscard]] std::string status() const;
  [[nodiscard]] sdbus::ObjectPath trackId() const;

  MpvPlayer& m_player;
  Actions m_actions;
  std::unique_ptr<sdbus::IObject> m_object;
  std::string m_lastStatus;
  std::string m_lastPath;
  std::string m_lastTitle;
  double m_lastDuration = 0.0;
  double m_lastVolume = -1.0;
  std::uint64_t m_track = 0;
};
