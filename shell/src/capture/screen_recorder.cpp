#include "capture/screen_recorder.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

  constexpr int kTestSourceWidth = 1280;
  constexpr int kTestSourceHeight = 720;

  // Sets the crop in buffer pixels once the stream's size is known; data is the recorder's CropGeometry.
  GstPadProbeReturn onCropCaps(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
      return GST_PAD_PROBE_OK;
    }
    const auto& geometry = *static_cast<const CropGeometry*>(data);
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    int width = 0;
    int height = 0;
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    if (!gst_structure_get_int(structure, "width", &width)
        || !gst_structure_get_int(structure, "height", &height)
        || geometry.stream.width <= 0
        || geometry.stream.height <= 0) {
      return GST_PAD_PROBE_OK;
    }
    // Logical to buffer pixels, rounded to even: NV12 chroma is subsampled 2x2.
    const double sx = static_cast<double>(width) / geometry.stream.width;
    const double sy = static_cast<double>(height) / geometry.stream.height;
    const auto even = [](double value) { return static_cast<int>(std::lround(value / 2.0)) * 2; };
    const LogicalRect& r = geometry.region;
    const int left = std::clamp(even(r.x * sx), 0, width - 2);
    const int top = std::clamp(even(r.y * sy), 0, height - 2);
    const int right = std::clamp(width - even((r.x + r.width) * sx), 0, width - left - 2);
    const int bottom = std::clamp(height - even((r.y + r.height) * sy), 0, height - top - 2);
    GstElement* crop = gst_pad_get_parent_element(pad);
    g_object_set(crop, "left", left, "top", top, "right", right, "bottom", bottom, nullptr);
    gst_object_unref(crop);
    return GST_PAD_PROBE_OK;
  }

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

std::string ScreenRecorder::toggle(std::optional<LogicalRect> region) {
  switch (m_state) {
  case State::Starting:
    return "a recording is starting";
  case State::Stopping:
    return "a recording is being saved";
  case State::Recording:
    setState(State::Stopping);
    m_stopDeadline = std::chrono::steady_clock::now() + kStopTimeout;
    gst_element_send_event(m_pipeline, gst_event_new_eos());
    return {};
  case State::Idle:
    break;
  }

  if (!gst_is_initialized()) {
    gst_init(nullptr, nullptr);
  }
  setState(State::Starting);
  m_region = region;
  if (std::getenv("NOCTALIA_RECORD_TEST_SOURCE") != nullptr) {
    m_crop.stream = {.x = 0, .y = 0, .width = kTestSourceWidth, .height = kTestSourceHeight};
    startPipeline([](const Make& make) {
      GstElement* src = make("videotestsrc");
      GstElement* caps = make("capsfilter");
      if (src != nullptr && caps != nullptr) {
        gst_util_set_object_arg(G_OBJECT(src), "pattern", "smpte");
        g_object_set(src, "is-live", TRUE, nullptr);
        GstCaps* raw = gst_caps_new_simple(
            "video/x-raw", "width", G_TYPE_INT, kTestSourceWidth, "height", G_TYPE_INT, kTestSourceHeight, "framerate",
            GST_TYPE_FRACTION, 30, 1, nullptr
        );
        g_object_set(caps, "caps", raw, nullptr);
        gst_caps_unref(raw);
      }
      return std::vector<GstElement*>{src, caps};
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
        const Results& properties = std::get<1>(list.front());
        m_crop.stream = {};
        if (const auto size = properties.find("size"); size != properties.end()) {
          const auto [width, height] = size->second.get<sdbus::Struct<std::int32_t, std::int32_t>>();
          m_crop.stream.width = width;
          m_crop.stream.height = height;
        }
        if (const auto position = properties.find("position"); position != properties.end()) {
          const auto [x, y] = position->second.get<sdbus::Struct<std::int32_t, std::int32_t>>();
          m_crop.stream.x = x;
          m_crop.stream.y = y;
        }
        try {
          sdbus::UnixFd fd;
          m_portal->callMethod("OpenPipeWireRemote")
              .onInterface(kScreenCast)
              .withArguments(sdbus::ObjectPath{m_sessionHandle}, Results{})
              .storeResultsTo(fd);
          // Each attempt's pipewiresrc owns and closes its own duplicate; the portal's fd closes when this returns.
          startPipeline([&fd, node = std::get<0>(list.front())](const Make& make) {
            GstElement* src = make("pipewiresrc");
            if (src != nullptr) {
              const std::string path = std::to_string(node);
              g_object_set(
                  src, "fd", ::dup(fd.get()), "path", path.c_str(), "do-timestamp", TRUE, "keepalive-time", 1000,
                  nullptr
              );
            }
            return std::vector<GstElement*>{src};
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

void ScreenRecorder::startPipeline(const Source& source) {
  if (m_region.has_value()) {
    const int left = std::max(m_region->x, m_crop.stream.x);
    const int top = std::max(m_region->y, m_crop.stream.y);
    const int right = std::min(m_region->x + m_region->width, m_crop.stream.x + m_crop.stream.width);
    const int bottom = std::min(m_region->y + m_region->height, m_crop.stream.y + m_crop.stream.height);
    if (right - left < 2 || bottom - top < 2) {
      finish("the region is not on the recorded display");
      return;
    }
    m_crop.region = {
        .x = left - m_crop.stream.x, .y = top - m_crop.stream.y, .width = right - left, .height = bottom - top
    };
  }
  m_path = recordingPath();
  std::string error = launch(source, true);
  const bool withAudio = error.empty();
  if (!withAudio) {
    kLog.warn("recording without audio: {}", error);
    error = launch(source, false);
  }
  if (!error.empty()) {
    finish(error);
    return;
  }
  setState(State::Recording);
  kLog.info("recording to {}{}", m_path, withAudio ? " with desktop audio" : "");
  m_notifications.addInternal(
      "Noctalia", withAudio ? "Recording started" : "Recording started without audio",
      "Run screen-record-toggle again to stop and save"
  );
}

std::string ScreenRecorder::launch(const Source& source, bool withAudio) {
  GstElement* pipeline = gst_pipeline_new("recorder");
  std::string missing;
  const Make make = [&](const char* factory) -> GstElement* {
    GstElement* element = gst_element_factory_make(factory, nullptr);
    if (element == nullptr) {
      if (missing.empty()) {
        missing = factory;
      }
      return nullptr;
    }
    gst_bin_add(GST_BIN(pipeline), element);
    return element;
  };
  const auto capsFilter = [&](GstCaps* caps) {
    GstElement* filter = make("capsfilter");
    if (filter != nullptr) {
      g_object_set(filter, "caps", caps, nullptr);
    }
    gst_caps_unref(caps);
    return filter;
  };
  const auto nv12 = [](const char* feature) {
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", nullptr);
    if (feature != nullptr) {
      gst_caps_set_features(caps, 0, gst_caps_features_new_single_static_str(feature));
    }
    return caps;
  };
  const auto link = [](const std::vector<GstElement*>& chain) {
    for (std::size_t i = 1; i < chain.size(); ++i) {
      if (chain[i - 1] == nullptr || chain[i] == nullptr || !gst_element_link(chain[i - 1], chain[i])) {
        return false;
      }
    }
    return true;
  };

  std::vector<GstElement*> video = source(make);
  if (m_region.has_value()) {
    // A region is cut out of a system-memory copy: vapostproc ignores crop metadata on VA surfaces.
    // Named at creation: a bin refuses to rename a child, and the caps probe finds the crop by this name.
    GstElement* crop = gst_element_factory_make("videocrop", "crop");
    if (crop != nullptr) {
      gst_bin_add(GST_BIN(pipeline), crop);
    } else if (missing.empty()) {
      missing = "videocrop";
    }
    video.insert(video.end(), {make("queue"), make("vapostproc"), capsFilter(nv12(nullptr)), crop});
  }
  GstElement* mux = make("mp4mux");
  GstElement* sink = make("filesink");
  if (sink != nullptr) {
    g_object_set(sink, "location", m_path.c_str(), nullptr);
  }
  video.insert(
      video.end(),
      {make("queue"), make("vapostproc"), capsFilter(nv12("memory:VAMemory")), make("vah264enc"), make("h264parse"),
       make("queue"), mux, sink}
  );
  bool linked = link(video);
  if (withAudio && linked) {
    // The default output's monitor: what the user hears.
    GstElement* src = make("pipewiresrc");
    if (src != nullptr) {
      GstStructure* props = gst_structure_new(
          "props", "stream.capture.sink", G_TYPE_STRING, "true", "node.name", G_TYPE_STRING, "noctalia-recorder",
          nullptr
      );
      g_object_set(src, "stream-properties", props, "do-timestamp", TRUE, nullptr);
      gst_structure_free(props);
    }
    GstCaps* audioCaps =
        gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, nullptr);
    linked = link(
        {src, make("queue"), make("audioconvert"), make("audioresample"), capsFilter(audioCaps), make("fdkaacenc"),
         make("queue"), mux}
    );
  }
  if (!linked) {
    gst_object_unref(pipeline);
    return missing.empty() ? std::string("the recording pipeline cannot link") : "missing GStreamer element " + missing;
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return "the encoder did not start";
  }
  m_pipeline = pipeline;
  if (GstElement* crop = gst_bin_get_by_name(GST_BIN(m_pipeline), "crop"); crop != nullptr) {
    GstPad* pad = gst_element_get_static_pad(crop, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, onCropCaps, &m_crop, nullptr);
    gst_object_unref(pad);
    gst_object_unref(crop);
  }
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
  setState(State::Idle);
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

void ScreenRecorder::setState(State state) {
  const bool wasActive = active();
  m_state = state;
  if (wasActive != active() && m_activeCallback) {
    m_activeCallback();
  }
}
