#pragma once

#include "app/desk_app.h"
#include "core/timer_manager.h"
#include "mpv_player.h"

#include <memory>
#include <string>

class Button;
class Label;
class MprisServer;
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
  void onPlayerChanged(bool seeked);
  void saveResume();
  void updateInhibit();
  void placeVideo();
  void showControls();
  void updateTransport();
  [[nodiscard]] std::unique_ptr<Node> buildContent(float width, float height);
  [[nodiscard]] static std::string resumeKey(const std::string& path);

  MpvPlayer m_mpv;
  std::unique_ptr<MprisServer> m_mpris;
  std::unique_ptr<VideoSurface> m_video;
  AppWindow* m_window = nullptr;
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
