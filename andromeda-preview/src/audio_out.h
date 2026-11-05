#pragma once

#include <chrono>
#include <memory>
#include <optional>

using GstElement = struct _GstElement;

namespace preview {

  // Plays the helper's PCM (F32LE, 48 kHz, stereo) from a pipe on PipeWire. The window never parses the container:
  // GStreamer here only reads raw samples (fdsrc ! rawaudioparse ! pipewiresink).
  class AudioOut {
  public:
    // Takes `readFd`. nullptr when an element is missing or the pipeline cannot start.
    static std::unique_ptr<AudioOut> start(int readFd);
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    void setPlaying(bool playing);
    // Stream time of the sample now audible; nullopt before the sink has a clock.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> position() const;

  private:
    AudioOut(GstElement* pipeline, int fd) : m_pipeline(pipeline), m_fd(fd) {}
    GstElement* m_pipeline;
    int m_fd;
  };

} // namespace preview
