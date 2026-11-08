// The AudioOut pipeline as a dlopen module (andromeda-preview-audio.so): apps that play media now and then (Files'
// Gallery) load GStreamer only when a video with sound plays, never at startup.
#include "audio_out.h"

#include <cstdint>

extern "C" {

__attribute__((visibility("default"))) void* andromeda_audio_start(int readFd) {
  return preview::AudioOut::start(readFd).release();
}

__attribute__((visibility("default"))) void andromeda_audio_stop(void* audio) {
  delete static_cast<preview::AudioOut*>(audio);
}

__attribute__((visibility("default"))) void andromeda_audio_set_playing(void* audio, int playing) {
  static_cast<preview::AudioOut*>(audio)->setPlaying(playing != 0);
}

// Stream time in ns of the sample now audible; -1 before the sink has a clock.
__attribute__((visibility("default"))) std::int64_t andromeda_audio_position_ns(void* audio) {
  const auto position = static_cast<preview::AudioOut*>(audio)->position();
  return position ? position->count() : -1;
}

} // extern "C"
