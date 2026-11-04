#pragma once

#include <cstdint>
#include <optional>

// App quality of service policy (docs/qos.md §3). Pure: no Wayland, D-Bus or clock; the caller passes the time.
namespace andromeda::qos {

  enum class Tier : std::uint8_t { Focus, Visible, Hidden, HiddenLong };

  // Window rule `qos`.
  enum class Rule : std::uint8_t { Normal, NeverThrottle, FreezeWhenHidden };

  // One app scope at one moment. With several windows, focused/visible hold when any window is.
  struct AppInputs {
    bool focused = false;
    bool visible = false; // some window is not suspended
    bool audio = false;
    bool call = false;
    bool idleInhibitor = false;
    bool screenCast = false;
    bool thawIntent = false; // the user starts to reach the app (switcher, dock hover, workspace swipe)
    Rule rule = Rule::Normal;
  };

  struct Decision {
    Tier tier = Tier::Visible;
    bool frozen = false;
    // When the tier can drop with no input change; nullopt = only an input change moves it.
    std::optional<std::uint64_t> recheckAtMs{};
  };

  inline constexpr std::uint64_t kHiddenDelayMs = 5'000;
  inline constexpr std::uint64_t kHiddenLongDelayMs = 10ULL * 60 * 1000; // in T2 before T3

  struct Delays {
    std::uint64_t hiddenMs = kHiddenDelayMs;
    std::uint64_t hiddenLongMs = kHiddenLongDelayMs;
  };

  class ScopePolicy {
  public:
    explicit ScopePolicy(Delays delays = {}) : m_delays(delays) {}
    // Promotes at once; demotes only after the delays, counted from the moment every window was hidden.
    Decision update(const AppInputs& inputs, bool onBattery, std::uint64_t nowMs);

  private:
    Delays m_delays;
    std::optional<std::uint64_t> m_hiddenSinceMs;
  };

} // namespace andromeda::qos
