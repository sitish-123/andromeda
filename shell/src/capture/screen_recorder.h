#pragma once

#include "capture/screencopy_capture.h"
#include "core/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class NotificationManager;
class SessionBus;

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

// Records the screen to ~/Videos as H.264 MP4 with the desktop's audio as AAC. The source is a ScreenCast portal
// stream (our portal's picker runs on the first recording of a shell session; later ones reuse the choice), encoded on
// the GPU through VA-API with GStreamer: pipewiresrc, vapostproc, vah264enc, mp4mux. Audio is the default output's
// monitor through fdkaacenc; if PipeWire refuses it, the recording is video only. ANDROMEDA_RECORD_TEST_SOURCE swaps
// the portal for videotestsrc so tests exercise encode and save without a portal.
// A region recording's stream in the global logical space, and the region clipped to it, relative to it.
struct CropGeometry {
  LogicalRect stream;
  LogicalRect region;
};

class ScreenRecorder final : public PollSource {
public:
  ScreenRecorder(SessionBus& bus, NotificationManager& notifications);
  ~ScreenRecorder() override;

  ScreenRecorder(const ScreenRecorder&) = delete;
  ScreenRecorder& operator=(const ScreenRecorder&) = delete;

  [[nodiscard]] bool active() const { return m_state != State::Idle; }
  // Runs when active() changes.
  void setActiveCallback(std::function<void()> callback) { m_activeCallback = std::move(callback); }
  // Starts a recording of the whole display, or of region (global logical coordinates) on it, or stops and saves the
  // current one. Returns an error message, empty on success.
  std::string toggle(std::optional<LogicalRect> region = std::nullopt);

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
  // Makes an element by factory name and adds it to the pipeline being built; nullptr when the factory is missing.
  using Make = std::function<GstElement*(const char* factory)>;
  // source(make) returns a fresh video source chain for each attempt.
  using Source = std::function<std::vector<GstElement*>(const Make& make)>;
  void startPipeline(const Source& source);
  // Builds the pipeline from elements (no gst-launch text, so the file path is never parsed); returns an error
  // message, empty on success.
  std::string launch(const Source& source, bool withAudio);
  void finish(const std::string& error);
  void closePortalSession();
  void watchPortalOwner();

  SessionBus& m_bus;
  NotificationManager& m_notifications;
  std::unique_ptr<sdbus::IProxy> m_portal;
  std::unique_ptr<sdbus::IProxy> m_request;
  sdbus::Slot m_portalOwnerWatch;
  std::string m_sessionHandle;
  std::string m_restoreToken;
  std::uint64_t m_tokenSeq = 0;
  State m_state = State::Idle;
  std::function<void()> m_activeCallback;
  void setState(State state);
  GstElement* m_pipeline = nullptr;
  GstBus* m_gstBus = nullptr;
  int m_busFd = -1;
  std::string m_path;
  std::optional<LogicalRect> m_region;
  CropGeometry m_crop;
  // Starting or Stopping ends by then.
  std::optional<std::chrono::steady_clock::time_point> m_deadline;
};
