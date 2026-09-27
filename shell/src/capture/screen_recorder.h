#pragma once

#include "app/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>

class NotificationManager;
class SessionBus;

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

// Records the screen to ~/Videos as H.264 MP4 with the desktop's audio as AAC. The source is a ScreenCast portal
// stream (our portal's picker runs on the first recording of a shell session; later ones reuse the choice), encoded on
// the GPU through VA-API with GStreamer: pipewiresrc, vapostproc, vah264enc, mp4mux. Audio is the default output's
// monitor through fdkaacenc; if PipeWire refuses it, the recording is video only. NOCTALIA_RECORD_TEST_SOURCE swaps
// the portal for videotestsrc so tests exercise encode and save without a portal.
class ScreenRecorder final : public PollSource {
public:
  ScreenRecorder(SessionBus& bus, NotificationManager& notifications);
  ~ScreenRecorder() override;

  ScreenRecorder(const ScreenRecorder&) = delete;
  ScreenRecorder& operator=(const ScreenRecorder&) = delete;

  [[nodiscard]] bool active() const { return m_state != State::Idle; }
  // Starts a recording, or stops and saves the current one. Returns an error message, empty on success.
  std::string toggle();

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  enum class State : std::uint8_t { Idle, Starting, Recording, Stopping };
  using Results = std::map<std::string, sdbus::Variant>;

  void portalRequest(
      const std::string& method, std::function<void(std::uint32_t, const Results&)> onResponse,
      std::function<void(sdbus::IProxy&, Results)> call
  );
  void selectSources();
  void startCast();
  // source() returns a fresh gst-launch video source for each attempt.
  void startPipeline(const std::function<std::string()>& source);
  // Starts a pipeline from a gst-launch description; returns an error message, empty on success.
  std::string launch(const std::string& description);
  void finish(const std::string& error);
  void closePortalSession();

  SessionBus& m_bus;
  NotificationManager& m_notifications;
  std::unique_ptr<sdbus::IProxy> m_portal;
  std::unique_ptr<sdbus::IProxy> m_request;
  std::string m_sessionHandle;
  std::string m_restoreToken;
  std::uint64_t m_tokenSeq = 0;
  State m_state = State::Idle;
  GstElement* m_pipeline = nullptr;
  GstBus* m_gstBus = nullptr;
  int m_busFd = -1;
  std::string m_path;
  std::optional<std::chrono::steady_clock::time_point> m_stopDeadline;
};
