#pragma once

#include "shell/settings/speaker_sound.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Flex;
class PipeWireService;
class Slider;

namespace settings {

  struct SettingsSoundContext {
    float scale = 1.0F;
    PipeWireService* audio = nullptr;
    // Set to the volume sliders, so an outside volume change moves them without a rebuild.
    Slider** outputSlider = nullptr;
    Slider** inputSlider = nullptr;
    // Filled with each program stream's slider, for the same reason.
    std::vector<std::pair<std::uint32_t, Slider*>>* programSliders = nullptr;
    // Speaker Sound card; hidden while status is null or the laptop has no internal speakers.
    SpeakerPhase speakerPhase = SpeakerPhase::Unloaded;
    const SpeakerStatus* speakerStatus = nullptr;
    std::string speakerError;
    bool speakerFineTuneOpen = false;
    // Runs andromeda-sound with these arguments (apply …, off, import-windows).
    std::function<void(std::vector<std::string>)> runSpeaker;
    std::function<void()> toggleSpeakerFineTune;
  };

  // Sound page (direction A, .agent/DESIGN.md): Output and Input cards with the device list (current one checked) and
  // its volume and mute.
  void addSettingsSound(Flex& content, const SettingsSoundContext& ctx);

  // Device ids and the defaults; the page rebuilds only when this changes, not on each volume step.
  [[nodiscard]] std::string soundDeviceSignature(const PipeWireService* audio);

} // namespace settings
