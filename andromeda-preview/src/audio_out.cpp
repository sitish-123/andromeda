#include "audio_out.h"

#include "core/log.h"

#include <gst/gst.h>
#include <unistd.h>

namespace preview {

  namespace {
    constexpr Logger kLog("preview");
  } // namespace

  std::unique_ptr<AudioOut> AudioOut::start(int readFd) {
    if (!gst_is_initialized()) {
      gst_init(nullptr, nullptr);
    }
    GstElement* pipeline = gst_pipeline_new("preview-audio");
    GstElement* src = gst_element_factory_make("fdsrc", nullptr);
    GstElement* parse = gst_element_factory_make("rawaudioparse", nullptr);
    GstElement* sink = gst_element_factory_make("pipewiresink", nullptr);
    if (src == nullptr || parse == nullptr || sink == nullptr) {
      kLog.warn("audio: missing fdsrc, rawaudioparse or pipewiresink");
      for (GstElement* e : {src, parse, sink}) {
        if (e != nullptr) {
          gst_object_unref(e);
        }
      }
      gst_object_unref(pipeline);
      close(readFd);
      return nullptr;
    }
    g_object_set(src, "fd", readFd, nullptr);
    g_object_set(parse, "use-sink-caps", FALSE, "sample-rate", 48000, "num-channels", 2, nullptr);
    gst_util_set_object_arg(G_OBJECT(parse), "pcm-format", "f32le");
    g_object_set(sink, "client-name", "Quick Look", nullptr);
    GstStructure* props = gst_structure_new("props", "media.role", G_TYPE_STRING, "Music", nullptr);
    g_object_set(sink, "stream-properties", props, nullptr);
    gst_structure_free(props);
    gst_bin_add_many(GST_BIN(pipeline), src, parse, sink, nullptr);
    auto out = std::unique_ptr<AudioOut>(new AudioOut(pipeline, readFd));
    if (!gst_element_link_many(src, parse, sink, nullptr)
        || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      kLog.warn("audio: the pipeline cannot start");
      return nullptr;
    }
    return out;
  }

  AudioOut::~AudioOut() {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    close(m_fd);
  }

  void AudioOut::setPlaying(bool playing) {
    gst_element_set_state(m_pipeline, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
  }

  std::optional<std::chrono::nanoseconds> AudioOut::position() const {
    gint64 pos = 0;
    if (!gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos) || pos < 0) {
      return std::nullopt;
    }
    return std::chrono::nanoseconds(pos);
  }

} // namespace preview
