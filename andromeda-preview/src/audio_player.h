#pragma once

#include <chrono>
#include <memory>
#include <optional>

namespace preview {

  // AudioOut through the andromeda-preview-audio module, loaded on first use: the caller does not link GStreamer.
  class AudioPlayer {
  public:
    // Takes `readFd` (the helper's PCM pipe). nullptr when the module or the pipeline is missing.
    static std::unique_ptr<AudioPlayer> start(int readFd);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    void setPlaying(bool playing);
    [[nodiscard]] std::optional<std::chrono::nanoseconds> position() const;

  private:
    explicit AudioPlayer(void* audio) : m_audio(audio) {}
    void* m_audio;
  };

} // namespace preview
