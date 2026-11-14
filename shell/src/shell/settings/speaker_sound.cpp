#include "shell/settings/speaker_sound.h"

#include <charconv>

namespace settings {

  namespace {
    double number(std::string_view v, double fallback) {
      double out = fallback;
      const auto [_, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
      return ec == std::errc{} ? out : fallback;
    }
  } // namespace

  std::optional<SpeakerStatus> parseSpeakerStatus(std::string_view out) {
    SpeakerStatus s;
    bool any = false;
    while (!out.empty()) {
      const auto nl = out.find('\n');
      const std::string_view line = out.substr(0, nl);
      out = nl == std::string_view::npos ? std::string_view{} : out.substr(nl + 1);
      const auto eq = line.find('=');
      if (eq == std::string_view::npos) {
        continue;
      }
      const std::string_view key = line.substr(0, eq);
      const std::string_view value = line.substr(eq + 1);
      any = true;
      if (key == "enabled") {
        s.enabled = value == "1";
      } else if (key == "source") {
        s.source = value;
      } else if (key == "profile") {
        s.profile = value;
      } else if (key == "loudness") {
        s.loudnessDb = number(value, s.loudnessDb);
      } else if (key == "bass") {
        s.bass = number(value, s.bass);
      } else if (key == "windows") {
        s.windows = value == "1";
      } else if (key == "speakers") {
        s.speakers = value == "1";
      } else if (key == "profiles") {
        for (std::string_view rest = value; !rest.empty();) {
          const auto comma = rest.find(',');
          if (const std::string_view p = rest.substr(0, comma); !p.empty()) {
            s.profiles.emplace_back(p);
          }
          rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
      }
    }
    return any ? std::optional(s) : std::nullopt;
  }

  std::optional<SpeakerPhase> speakerTransition(SpeakerPhase from, SpeakerEvent event) {
    using P = SpeakerPhase;
    using E = SpeakerEvent;
    switch (event) {
    case E::Open:
      return from == P::Unloaded || from == P::Ready || from == P::Unavailable || from == P::ImportFailed ||
                   from == P::ApplyFailed
                 ? std::optional(P::Loading)
                 : std::nullopt;
    case E::Close:
      return from == P::Ready || from == P::Unavailable || from == P::ImportFailed || from == P::ApplyFailed
                 ? std::optional(P::Unloaded)
                 : std::nullopt;
    case E::StatusOk:
      return from == P::Loading ? std::optional(P::Ready) : std::nullopt;
    case E::StatusFailed:
      return from == P::Loading ? std::optional(P::Unavailable) : std::nullopt;
    case E::Apply:
      return from == P::Ready || from == P::ImportFailed || from == P::ApplyFailed ? std::optional(P::Applying) : std::nullopt;
    case E::Import:
      return from == P::Ready || from == P::ImportFailed || from == P::ApplyFailed ? std::optional(P::Importing) : std::nullopt;
    case E::CommandDone:
      return from == P::Applying || from == P::Importing ? std::optional(P::Loading) : std::nullopt;
    case E::ImportFailed:
      return from == P::Importing ? std::optional(P::ImportFailed) : std::nullopt;
    case E::ApplyFailed:
      return from == P::Applying ? std::optional(P::ApplyFailed) : std::nullopt;
    }
    return std::nullopt;
  }

  bool speakerBusy(SpeakerPhase phase) {
    return phase == SpeakerPhase::Loading || phase == SpeakerPhase::Applying || phase == SpeakerPhase::Importing;
  }

  const char* speakerImportErrorKey(std::string_view err) {
    if (err.contains("no Windows partition")) {
      return "settings.sound.windows-none";
    }
    return err.contains("apply failed") ? "settings.sound.apply-error" : "settings.sound.windows-error";
  }

} // namespace settings
