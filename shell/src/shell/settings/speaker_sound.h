#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace settings {

  // `andromeda-sound status` output.
  struct SpeakerStatus {
    bool enabled = false;
    // "basic" (our curve) or the vendor the tuning came from ("Dolby").
    std::string source = "basic";
    std::string profile = "music";
    double loudnessDb = 4.0;
    double bass = 5.0;
    std::vector<std::string> profiles;
    bool windows = false;
    bool speakers = false;

    [[nodiscard]] bool basic() const { return source == "basic"; }
  };

  [[nodiscard]] std::optional<SpeakerStatus> parseSpeakerStatus(std::string_view out);

  // The Speaker Sound card's one mode. At most one andromeda-sound command runs: Apply and Import start one only
  // from Ready, ImportFailed or ApplyFailed.
  enum class SpeakerPhase { Unloaded, Loading, Ready, Unavailable, Applying, Importing, ImportFailed, ApplyFailed };
  enum class SpeakerEvent { Open, Close, StatusOk, StatusFailed, Apply, Import, CommandDone, ImportFailed, ApplyFailed };

  // The next phase, or nullopt when the event is not valid in `from`.
  [[nodiscard]] std::optional<SpeakerPhase> speakerTransition(SpeakerPhase from, SpeakerEvent event);

} // namespace settings
