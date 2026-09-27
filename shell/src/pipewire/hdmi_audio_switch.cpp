#include "pipewire/hdmi_audio_switch.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "pipewire/pipewire_service.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("hdmi-audio");
  // Sinks and their routes arriving while PipeWire enumerates them, at start or after a reconnect, are not plugs.
  constexpr auto kEnumerationGrace = std::chrono::seconds(5);

  bool isDisplayPort(const AudioNode& sink) { return sink.portType == "hdmi" || sink.portType == "displayport"; }

} // namespace

void HdmiAudioSwitch::onAudioStateChanged(PipeWireService& pipewire, bool enabled) {
  const AudioState& state = pipewire.state();
  const auto now = std::chrono::steady_clock::now();
  if (state.sinks.empty()) {
    m_hadSinks = false;
    m_connected.clear();
    return;
  }
  if (!m_hadSinks) {
    m_hadSinks = true;
    m_populatedAt = now;
  }

  std::uint32_t plugged = 0;
  std::unordered_map<std::uint32_t, bool> connected;
  for (const AudioNode& sink : state.sinks) {
    const bool isConnected = isDisplayPort(sink) && sink.portConnected;
    connected[sink.id] = isConnected;
    const auto previous = m_connected.find(sink.id);
    const bool wasConnected = previous != m_connected.end() && previous->second;
    if (isConnected && !wasConnected && plugged == 0) {
      plugged = sink.id;
    }
  }
  m_connected = std::move(connected);
  if (!enabled || plugged == 0 || now - m_populatedAt < kEnumerationGrace) {
    return;
  }

  const AudioNode* current = pipewire.defaultSink();
  const auto target = std::ranges::find(state.sinks, plugged, &AudioNode::id);
  if (current == nullptr
      || current->deviceId == 0
      || current->deviceId != target->deviceId
      || (current->portType != "speaker" && current->portType != "headphones")) {
    return;
  }
  kLog.info(
      "'{}' plugged in, moving the default output off '{}'", audioDeviceLabel(*target), audioDeviceLabel(*current)
  );
  DeferredCall::callLater([&pipewire, plugged]() { pipewire.setDefaultSink(plugged); });
}
