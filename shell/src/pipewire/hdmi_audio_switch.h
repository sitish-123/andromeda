#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

class PipeWireService;

// Makes a newly plugged HDMI or DisplayPort sink the default output while sound plays on the same card's speaker or
// headphones. It acts on plug transitions only, so choosing the speaker again while the display stays connected
// sticks until the next plug. Unplugging needs nothing here: WirePlumber drops sinks without an available route from
// its default candidates and falls back on its own.
class HdmiAudioSwitch {
public:
  void onAudioStateChanged(PipeWireService& pipewire, bool enabled);

private:
  std::unordered_map<std::uint32_t, bool> m_connected;
  std::chrono::steady_clock::time_point m_populatedAt;
  bool m_hadSinks = false;
};
