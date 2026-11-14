#include "shell/settings/speaker_sound.h"

#include <print>
#include <random>

using settings::SpeakerEvent;
using settings::SpeakerPhase;

namespace {
  bool busy(SpeakerPhase p) {
    return p == SpeakerPhase::Applying || p == SpeakerPhase::Importing || p == SpeakerPhase::Loading;
  }
} // namespace

int main() {
  int failures = 0;
  const auto s = settings::parseSpeakerStatus(
      "enabled=1\nsource=Dolby\nprofile=movie\nloudness=6.5\nbass=3\nprofiles=music,movie\nwindows=1\nspeakers=1\n"
  );
  if (!s || !s->enabled || s->basic() || s->profile != "movie" || s->loudnessDb != 6.5 || s->bass != 3.0 ||
      s->profiles.size() != 2 || !s->windows || !s->speakers) {
    std::println(stderr, "speaker_sound_test: status line parse");
    ++failures;
  }
  if (settings::parseSpeakerStatus("error: no speaker sink").has_value()) {
    std::println(stderr, "speaker_sound_test: an error message must not parse as a status");
    ++failures;
  }

  // Random event sequences: a command (apply, import, status) never starts while another runs, and a busy phase
  // always ends through its own completion event.
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> pick(0, 7);
  for (int run = 0; run < 2000; ++run) {
    SpeakerPhase phase = SpeakerPhase::Unloaded;
    for (int step = 0; step < 50; ++step) {
      const auto event = static_cast<SpeakerEvent>(pick(rng));
      const auto next = settings::speakerTransition(phase, event);
      if (!next) {
        continue;
      }
      const bool starts = event == SpeakerEvent::Apply || event == SpeakerEvent::Import || event == SpeakerEvent::Open;
      if (starts && busy(phase)) {
        std::println(stderr, "speaker_sound_test: a command started while one ran (phase {})", static_cast<int>(phase));
        ++failures;
      }
      if (busy(phase) && busy(*next) && event != SpeakerEvent::CommandDone) {
        std::println(stderr, "speaker_sound_test: busy phase {} left without completion", static_cast<int>(phase));
        ++failures;
      }
      phase = *next;
    }
  }
  return failures == 0 ? 0 : 1;
}
