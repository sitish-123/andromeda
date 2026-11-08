#include "audio_player.h"

#include "core/log.h"

#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <unistd.h>

namespace preview {

  namespace {
    constexpr Logger kLog("audio");

    struct Module {
      void* (*start)(int) = nullptr;
      void (*stop)(void*) = nullptr;
      void (*setPlaying)(void*, int) = nullptr;
      std::int64_t (*positionNs)(void*) = nullptr;
    };

    // Installed next to the other Andromeda helpers; a build tree keeps it in the andromeda-preview subproject dir.
    const Module* module() {
      static Module loaded;
      static std::once_flag once;
      std::call_once(once, [] {
        std::array<char, 4096> exe{};
        const ssize_t n = ::readlink("/proc/self/exe", exe.data(), exe.size() - 1);
        const std::filesystem::path dir = n > 0 ? std::filesystem::path(std::string(exe.data(), static_cast<std::size_t>(n))).parent_path()
                                                : std::filesystem::path();
        for (const std::filesystem::path& candidate :
             {std::filesystem::path(ANDROMEDA_PREVIEW_AUDIO_PATH), dir / "subprojects/andromeda-preview/andromeda-preview-audio.so",
              dir / "andromeda-preview-audio.so"}) {
          void* handle = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
          if (handle == nullptr) {
            continue;
          }
          loaded.start = reinterpret_cast<void* (*)(int)>(::dlsym(handle, "andromeda_audio_start"));
          loaded.stop = reinterpret_cast<void (*)(void*)>(::dlsym(handle, "andromeda_audio_stop"));
          loaded.setPlaying = reinterpret_cast<void (*)(void*, int)>(::dlsym(handle, "andromeda_audio_set_playing"));
          loaded.positionNs = reinterpret_cast<std::int64_t (*)(void*)>(::dlsym(handle, "andromeda_audio_position_ns"));
          if (loaded.start != nullptr && loaded.stop != nullptr && loaded.setPlaying != nullptr && loaded.positionNs != nullptr) {
            kLog.info("audio module {}", candidate.string());
            return;
          }
          loaded = {};
        }
        kLog.warn("audio: no andromeda-preview-audio module; video plays muted");
      });
      return loaded.start != nullptr ? &loaded : nullptr;
    }
  } // namespace

  std::unique_ptr<AudioPlayer> AudioPlayer::start(int readFd) {
    const Module* m = module();
    if (m == nullptr) {
      ::close(readFd);
      return nullptr;
    }
    void* audio = m->start(readFd);
    return audio != nullptr ? std::unique_ptr<AudioPlayer>(new AudioPlayer(audio)) : nullptr;
  }

  AudioPlayer::~AudioPlayer() { module()->stop(m_audio); }

  void AudioPlayer::setPlaying(bool playing) { module()->setPlaying(m_audio, playing ? 1 : 0); }

  std::optional<std::chrono::nanoseconds> AudioPlayer::position() const {
    const std::int64_t ns = module()->positionNs(m_audio);
    return ns >= 0 ? std::optional(std::chrono::nanoseconds(ns)) : std::nullopt;
  }

} // namespace preview
