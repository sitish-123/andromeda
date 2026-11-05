#pragma once

#include "core/poll_source.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct MpvLib;
struct mpv_handle;
struct mpv_event_property;

// One libmpv instance. mpv wakes the loop through an eventfd; dispatch() drains its events on the loop thread and
// keeps a snapshot of the observed properties.
class MpvPlayer final : public PollSource {
public:
  struct State {
    std::string path;
    std::string title;
    double duration = 0.0; // seconds; 0 until known
    double position = 0.0;
    bool paused = true;
    bool idle = true;      // nothing loaded
    bool hasVideo = false;
    std::int64_t width = 0; // video size in pixels
    std::int64_t height = 0;
    double volume = 100.0;
    bool muted = false;
    bool eof = false;
    std::int64_t queuePos = -1; // index in mpv's playlist
    std::int64_t queueCount = 0;
    int subtitleTracks = 0;
    std::string subtitle; // the shown track's language or title; empty when off
  };

  MpvPlayer();
  ~MpvPlayer() override;

  MpvPlayer(const MpvPlayer&) = delete;
  MpvPlayer& operator=(const MpvPlayer&) = delete;

  // Loads libmpv and creates the core; false when libmpv is missing. vo=libmpv: frames go to a render context.
  bool initialize();
  [[nodiscard]] bool ready() const noexcept { return m_mpv != nullptr; }
  [[nodiscard]] mpv_handle* handle() const noexcept { return m_mpv; }
  [[nodiscard]] const MpvLib* lib() const noexcept { return m_lib; }

  void load(const std::string& path, double startSeconds);
  // A queue (album, playlist) as mpv's playlist: gapless joins, ReplayGain per album or per track.
  void loadList(const std::vector<std::string>& paths, bool albumGain, std::size_t start = 0);
  void next();
  void previous();
  void setPaused(bool paused);
  void togglePause() { setPaused(!m_state.paused); }
  void seekTo(double seconds);
  void seekBy(double seconds);
  void setVolume(double volume);
  void stop();
  // Off -> each subtitle track -> off.
  void cycleSubtitles();

  [[nodiscard]] const State& state() const noexcept { return m_state; }
  // After any property change; `seeked` is true for a seek the user or MPRIS made (MPRIS Seeked).
  void setChangedCallback(std::function<void(bool seeked)> callback) { m_onChanged = std::move(callback); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  void onProperty(std::uint64_t id, const mpv_event_property& property);
  void command(std::initializer_list<const char*> args);

  const MpvLib* m_lib = nullptr;
  mpv_handle* m_mpv = nullptr;
  int m_wakeFd = -1;
  bool m_voConfigured = false;
  bool m_albumArt = false;
  bool m_seekPending = false;
  std::string m_subId;
  std::string m_subLang;
  std::string m_subTitle;
  void countSubtitles();
  State m_state;
  std::function<void(bool)> m_onChanged;
};
