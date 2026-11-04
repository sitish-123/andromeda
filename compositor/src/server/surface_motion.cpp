#include "server/surface_motion.h"

#include "design_tokens.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace andromeda {

  namespace {

    using Curve = SurfaceMotion::Curve;

    // Wrap-safe: CLOCK_MONOTONIC ms in a uint32_t wraps after 49 days.
    double secondsBetween(uint32_t fromMs, uint32_t toMs) { return static_cast<double>(toMs - fromMs) / 1000.0; }

    // 0..1 at `cycles` periods since the loop started.
    double waveLevel(SurfaceMotion::Wave wave, double cycles, uint32_t seed) {
      constexpr double kTau = 2.0 * std::numbers::pi;
      if (wave == SurfaceMotion::Wave::Sine) {
        return 0.5 - 0.5 * std::cos(kTau * cycles);
      }
      // Two incommensurate sines per seed: no visible repeat, smooth at any frame rate.
      const double s = static_cast<double>(seed);
      const double carrier = std::sin(kTau * cycles * (1.0 + 0.33 * static_cast<double>(seed % 5)) + s * 2.1);
      const double envelope = 0.65 + 0.35 * std::sin(kTau * cycles * 0.45 + s);
      return std::clamp(0.5 + 0.5 * carrier * envelope, 0.0, 1.0);
    }

  } // namespace

  bool SurfaceMotion::isSpring(Curve curve) { return curve != Curve::None; }

  desk::motion::SpringConfig SurfaceMotion::springConfig(Curve curve) {
    const Tokens::Spring* token = &Tokens::springMove;
    switch (curve) {
    case Curve::SpringSnappy: token = &Tokens::springSnap; break;
    case Curve::SpringBouncy:
    case Curve::SpringOpen: token = &Tokens::springOpen; break;
    case Curve::SpringClose: token = &Tokens::springClose; break;
    case Curve::SpringIslandExpand: token = &Tokens::springMorph; break;
    case Curve::SpringIslandCollapse: token = &Tokens::springCollapse; break;
    case Curve::SpringPage: token = &Tokens::springPage; break;
    case Curve::SpringPress: token = &Tokens::springPress; break;
    case Curve::SpringRelease: token = &Tokens::springRelease; break;
    case Curve::EaseNormal:
    case Curve::EaseSlow:
    case Curve::SpringEffects: token = &Tokens::springFade; break;
    case Curve::EaseFast:
    case Curve::SpringEffectsFast: token = &Tokens::springFadeOut; break;
    default: break;
    }
    return {.damping = token->damping, .stiffness = token->stiffness};
  }

  SurfaceMotion::SurfaceMotion() {
    m_values[static_cast<std::size_t>(Property::Scale)] = {1.0, 0.0, 0.0, 0.0};
    m_values[static_cast<std::size_t>(Property::Opacity)] = {1.0, 0.0, 0.0, 0.0};
  }

  const SurfaceMotion::Value& SurfaceMotion::value(Property property) const {
    return m_values[static_cast<std::size_t>(property)];
  }

  const SurfaceMotion::Value& SurfaceMotion::target(Property property) const {
    const Track& track = m_tracks[static_cast<std::size_t>(property)];
    return track.running ? track.to : value(property);
  }

  bool SurfaceMotion::animating() const {
    for (const Track& track : m_tracks) {
      if (track.running) {
        return true;
      }
    }
    return false;
  }

  bool SurfaceMotion::settling() const {
    return std::ranges::any_of(m_tracks, [](const Track& track) { return track.running && !track.loop; });
  }

  bool SurfaceMotion::looping() const {
    return std::ranges::any_of(m_tracks, [](const Track& track) { return track.running && track.loop; });
  }

  void SurfaceMotion::loop(
      uint32_t serial, Property property, Wave wave, uint32_t periodMs, uint32_t seed, const Value& high
  ) {
    const auto index = static_cast<std::size_t>(property);
    Track& track = m_tracks[index];
    // A loop that replaces a loop keeps its low end, not the mid-cycle value.
    const Value low = track.running && track.loop ? track.low : m_values[index];
    finish(track, Reason::Retargeted);
    track = Track{};
    track.serial = serial;
    track.running = true;
    track.loop = true;
    track.wave = wave;
    track.periodMs = std::clamp<uint32_t>(periodMs, 100, 60000);
    track.seed = seed;
    track.low = low;
    track.to = high;
  }

  void SurfaceMotion::finish(Track& track, Reason reason) {
    if (!track.running) {
      return;
    }
    track.running = false;
    m_events.push_back({.kind = Event::Kind::Done, .serial = track.serial, .reason = reason});
  }

  void SurfaceMotion::set(Property property, const Value& value) {
    const auto index = static_cast<std::size_t>(property);
    finish(m_tracks[index], Reason::Cancelled);
    m_values[index] = value;
  }

  void SurfaceMotion::animate(uint32_t serial, Property property, Curve curve, const Value& to) {
    const auto index = static_cast<std::size_t>(property);
    Track& track = m_tracks[index];
    const Value& current = m_values[index];
    if (curve == Curve::None) {
      finish(track, Reason::Cancelled);
      m_values[index] = to;
      m_events.push_back({.kind = Event::Kind::Started, .serial = serial, .reason = Reason::Finished});
      m_events.push_back({.kind = Event::Kind::Done, .serial = serial, .reason = Reason::Finished});
      return;
    }

    const bool keepVelocity = track.running && !track.loop && isSpring(track.curve) && isSpring(curve);
    finish(track, Reason::Retargeted);
    for (std::size_t i = 0; i < 4; ++i) {
      if (keepVelocity) {
        const desk::motion::Spring& old = track.springs[i];
        track.springs[i] = desk::motion::Spring(old.value(), to[i], springConfig(curve), old.velocity());
      } else {
        track.springs[i] = desk::motion::Spring(current[i], to[i], springConfig(curve));
      }
    }
    track.loop = false;
    track.curve = curve;
    track.serial = serial;
    track.running = true;
    track.started = false;
    track.stepOnStart = keepVelocity;
    track.to = to;
  }

  bool SurfaceMotion::tick(uint32_t nowMs) {
    const double dt = m_lastTickMs ? secondsBetween(*m_lastTickMs, nowMs) : 0.0;
    m_lastTickMs = nowMs;
    bool changed = false;
    for (std::size_t index = 0; index < kPropertyCount; ++index) {
      Track& track = m_tracks[index];
      if (!track.running) {
        continue;
      }
      if (track.loop) {
        if (!track.started) {
          track.started = true;
          track.startMs = nowMs;
          m_events.push_back({.kind = Event::Kind::Started, .serial = track.serial, .reason = Reason::Finished});
        }
        const double cycles = static_cast<double>(nowMs - track.startMs) / static_cast<double>(track.periodMs);
        const double t = waveLevel(track.wave, cycles, track.seed);
        for (std::size_t i = 0; i < 4; ++i) {
          m_values[index][i] = track.low[i] + (track.to[i] - track.low[i]) * t;
        }
        changed = true;
        continue;
      }
      if (!track.started) {
        track.started = true;
        m_events.push_back({.kind = Event::Kind::Started, .serial = track.serial, .reason = Reason::Finished});
        if (!track.stepOnStart) {
          continue;
        }
      }

      Value& value = m_values[index];
      bool settled = true;
      for (std::size_t i = 0; i < 4; ++i) {
        value[i] = track.springs[i].step(dt);
        settled = settled && track.springs[i].settled();
      }
      changed = true;
      if (settled) {
        value = track.to;
        finish(track, Reason::Finished);
      }
    }
    return changed;
  }

  std::vector<SurfaceMotion::Event> SurfaceMotion::takeEvents() { return std::exchange(m_events, {}); }

} // namespace andromeda
