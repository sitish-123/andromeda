#include "capture/screen_recorder.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <glib.h>
#include <gst/gst.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("screen-recorder");

  const sdbus::ServiceName kPortalBusName{"org.freedesktop.portal.Desktop"};
  const sdbus::ObjectPath kPortalPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kScreenCast = "org.freedesktop.portal.ScreenCast";
  constexpr auto kRequest = "org.freedesktop.portal.Request";
  constexpr auto kSession = "org.freedesktop.portal.Session";
  constexpr std::uint32_t kSourceMonitor = 1;
  constexpr std::uint32_t kCursorEmbedded = 2;
  constexpr std::uint32_t kPersistWhileRunning = 1;
  constexpr std::uint32_t kResponseCancelled = 1;
  // EOS drains the encoder; a pipeline that has not finished by then is torn down and its file may be unplayable.
  constexpr auto kStopTimeout = std::chrono::seconds(5);

  constexpr auto kEncode =
      " ! queue ! vapostproc ! video/x-raw(memory:VAMemory),format=NV12 ! vah264enc ! h264parse ! queue ! mux."
      " mp4mux name=mux ! filesink location=\"{}\"";
  // The default output's monitor: what the user hears.
  constexpr auto kDesktopAudio =
      " pipewiresrc stream-properties=\"props,stream.capture.sink=true,node.name=noctalia-recorder\" do-timestamp=true"
      " ! queue ! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! fdkaacenc ! queue ! mux.";

  std::filesystem::path videosDir() {
    const char* dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    if (dir != nullptr) {
      return dir;
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/tmp") / "Videos";
  }

  std::string recordingPath() {
    const std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof(name), "Recording_%Y-%m-%d_%H.%M.%S.mp4", std::localtime(&now));
    std::filesystem::create_directories(videosDir());
    return (videosDir() / name).string();
  }

} // namespace

ScreenRecorder::ScreenRecorder(SessionBus& bus, NotificationManager& notifications)
    : m_bus(bus), m_notifications(notifications) {}

ScreenRecorder::~ScreenRecorder() {
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_gstBus);
    gst_object_unref(m_pipeline);
  }
  closePortalSession();
}

std::string ScreenRecorder::toggle() {
  switch (m_state) {
  case State::Starting:
    return "a recording is starting";
  case State::Stopping:
    return "a recording is being saved";
  case State::Recording:
    m_state = State::Stopping;
    m_stopDeadline = std::chrono::steady_clock::now() + kStopTimeout;
    gst_element_send_event(m_pipeline, gst_event_new_eos());
    return {};
  case State::Idle:
    break;
  }

  if (!gst_is_initialized()) {
    gst_init(nullptr, nullptr);
  }
  m_state = State::Starting;
  if (std::getenv("NOCTALIA_RECORD_TEST_SOURCE") != nullptr) {
    startPipeline([]() {
      return std::string("videotestsrc is-live=true ! video/x-raw,width=1280,height=720,framerate=30/1");
    });
    return m_state == State::Recording ? std::string{} : std::string("the recording pipeline did not start");
  }

  try {
    m_portal = sdbus::createProxy(m_bus.connection(), kPortalBusName, kPortalPath);
    portalRequest(
        "CreateSession",
        [this](std::uint32_t response, const Results& results) {
          const auto handle = results.find("session_handle");
          if (response != 0 || handle == results.end()) {
            finish(response == kResponseCancelled ? "" : "the portal refused a session");
            return;
          }
          m_sessionHandle = handle->second.get<std::string>();
          selectSources();
        },
        [this](sdbus::IProxy& portal, Results options) {
          options["session_handle_token"] = sdbus::Variant{std::format("noctalia_rec{}", m_tokenSeq)};
          portal.callMethodAsync("CreateSession")
              .onInterface(kScreenCast)
              .withArguments(options)
              .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::ObjectPath /*request*/) {
                if (error.has_value()) {
                  finish(error->what());
                }
              });
        }
    );
  } catch (const sdbus::Error& e) {
    finish(e.what());
    return e.what();
  }
  return {};
}

void ScreenRecorder::portalRequest(
    const std::string& method, std::function<void(std::uint32_t, const Results&)> onResponse,
    std::function<void(sdbus::IProxy&, Results)> call
) {
  const std::string token = std::format("noctalia_rec{}", ++m_tokenSeq);
  std::string sender = m_bus.connection().getUniqueName().substr(1);
  std::ranges::replace(sender, '.', '_');
  // Subscribed before the call, so a fast Response is never missed.
  m_request = sdbus::createProxy(
      m_bus.connection(), kPortalBusName,
      sdbus::ObjectPath{std::format("/org/freedesktop/portal/desktop/request/{}/{}", sender, token)}
  );
  m_request->uponSignal("Response")
      .onInterface(kRequest)
      .call([this, onResponse, method](std::uint32_t response, const Results& results) {
        kLog.debug("{} answered {}", method, response);
        // The request proxy is running this handler; replace it only once the handler has returned.
        DeferredCall::callLater([this, onResponse, response, results]() { onResponse(response, results); });
      });
  call(*m_portal, Results{{"handle_token", sdbus::Variant{token}}});
}

void ScreenRecorder::selectSources() {
  portalRequest(
      "SelectSources",
      [this](std::uint32_t response, const Results& /*results*/) {
        if (response != 0) {
          finish(response == kResponseCancelled ? "" : "the portal refused the source selection");
          return;
        }
        startCast();
      },
      [this](sdbus::IProxy& portal, Results options) {
        options["types"] = sdbus::Variant{kSourceMonitor};
        options["cursor_mode"] = sdbus::Variant{kCursorEmbedded};
        options["persist_mode"] = sdbus::Variant{kPersistWhileRunning};
        if (!m_restoreToken.empty()) {
          options["restore_token"] = sdbus::Variant{m_restoreToken};
        }
        portal.callMethodAsync("SelectSources")
            .onInterface(kScreenCast)
            .withArguments(sdbus::ObjectPath{m_sessionHandle}, options)
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::ObjectPath /*request*/) {
              if (error.has_value()) {
                finish(error->what());
              }
            });
      }
  );
}

void ScreenRecorder::startCast() {
  portalRequest(
      "Start",
      [this](std::uint32_t response, const Results& results) {
        if (response != 0) {
          finish(response == kResponseCancelled ? "" : "the portal did not start the cast");
          return;
        }
        if (const auto token = results.find("restore_token"); token != results.end()) {
          m_restoreToken = token->second.get<std::string>();
        }
        const auto streams = results.find("streams");
        if (streams == results.end()) {
          finish("the portal returned no stream");
          return;
        }
        const auto list = streams->second.get<std::vector<sdbus::Struct<std::uint32_t, Results>>>();
        if (list.empty()) {
          finish("the portal returned no stream");
          return;
        }
        try {
          sdbus::UnixFd fd;
          m_portal->callMethod("OpenPipeWireRemote")
              .onInterface(kScreenCast)
              .withArguments(sdbus::ObjectPath{m_sessionHandle}, Results{})
              .storeResultsTo(fd);
          // Each attempt's pipewiresrc owns and closes its own duplicate; the portal's fd closes when this returns.
          startPipeline([&fd, node = std::get<0>(list.front())]() {
            return std::format(
                "pipewiresrc fd={} path={} do-timestamp=true keepalive-time=1000", ::dup(fd.get()), node
            );
          });
        } catch (const sdbus::Error& e) {
          finish(e.what());
        }
      },
      [this](sdbus::IProxy& portal, Results options) {
        portal.callMethodAsync("Start")
            .onInterface(kScreenCast)
            .withArguments(sdbus::ObjectPath{m_sessionHandle}, std::string{}, options)
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, sdbus::ObjectPath /*request*/) {
              if (error.has_value()) {
                finish(error->what());
              }
            });
      }
  );
}

void ScreenRecorder::startPipeline(const std::function<std::string()>& source) {
  m_path = recordingPath();
  const auto video = [&]() { return source() + std::format(kEncode, m_path); };
  std::string error = launch(video() + kDesktopAudio);
  const bool withAudio = error.empty();
  if (!withAudio) {
    kLog.warn("recording without audio: {}", error);
    error = launch(video());
  }
  if (!error.empty()) {
    finish(error);
    return;
  }
  m_state = State::Recording;
  kLog.info("recording to {}{}", m_path, withAudio ? " with desktop audio" : "");
  m_notifications.addInternal(
      "Noctalia", withAudio ? "Recording started" : "Recording started without audio",
      "Run screen-record-toggle again to stop and save"
  );
}

std::string ScreenRecorder::launch(const std::string& description) {
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
  if (pipeline == nullptr) {
    std::string text = error != nullptr ? error->message : "invalid pipeline";
    g_clear_error(&error);
    return text;
  }
  g_clear_error(&error);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return "the encoder did not start";
  }
  m_pipeline = pipeline;
  m_gstBus = gst_element_get_bus(m_pipeline);
  GPollFD pollFd{};
  gst_bus_get_pollfd(m_gstBus, &pollFd);
  m_busFd = pollFd.fd;
  return {};
}

void ScreenRecorder::finish(const std::string& error) {
  if (m_pipeline != nullptr) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_gstBus);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
    m_gstBus = nullptr;
    m_busFd = -1;
  }
  closePortalSession();
  m_request.reset();
  m_stopDeadline.reset();
  const bool saved = m_state == State::Stopping && error.empty();
  m_state = State::Idle;
  if (saved) {
    kLog.info("recording saved to {}", m_path);
    m_notifications.addInternal("Noctalia", "Recording saved", m_path);
  } else if (!error.empty()) {
    kLog.warn("recording failed: {}", error);
    m_notifications.addInternal("Noctalia", "Recording failed", error);
  }
}

void ScreenRecorder::closePortalSession() {
  if (!m_sessionHandle.empty()) {
    try {
      sdbus::createProxy(m_bus.connection(), kPortalBusName, sdbus::ObjectPath{m_sessionHandle})
          ->callMethod("Close")
          .onInterface(kSession);
    } catch (const sdbus::Error& e) {
      kLog.debug("closing the portal session failed: {}", e.what());
    }
    m_sessionHandle.clear();
  }
}

int ScreenRecorder::pollTimeoutMs() const {
  if (!m_stopDeadline.has_value()) {
    return -1;
  }
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(*m_stopDeadline - std::chrono::steady_clock::now()).count();
  return static_cast<int>(std::max<std::int64_t>(0, remaining));
}

void ScreenRecorder::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_busFd >= 0) {
    fds.push_back({.fd = m_busFd, .events = POLLIN, .revents = 0});
  }
}

void ScreenRecorder::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  if (m_stopDeadline.has_value() && std::chrono::steady_clock::now() >= *m_stopDeadline) {
    finish("the encoder did not finish in time; the file may be incomplete");
    return;
  }
  while (m_gstBus != nullptr) {
    GstMessage* message = gst_bus_pop(m_gstBus);
    if (message == nullptr) {
      return;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      gst_message_unref(message);
      finish({});
      return;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gst_message_parse_error(message, &error, nullptr);
      const std::string text = error != nullptr ? error->message : "pipeline error";
      g_clear_error(&error);
      gst_message_unref(message);
      finish(text);
      return;
    }
    gst_message_unref(message);
  }
}
