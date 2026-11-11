#include "shell/live_activity/island_morph.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Drives the island morph through random event sequences against a model of its surroundings: drivers are
// AnimationManager timers (4 s cap, a stalled frame jumps past it), the compositor sends `started` late, stale or
// never, and the mirror springs settle 600 ms after they start. The glue follows LiveActivityIsland's mirrored path.
namespace {

  enum class Face : std::uint8_t { Rest, Osd, Banner };
  constexpr int kFaces = 3;
  constexpr int kDriverMaxMs = 4000; // kMirrorMaxMs
  constexpr int kStartWaitMs = 500;  // kStartWait
  constexpr int kSettleMs = 600;

  struct Env {
    int now = 0;
    std::uint32_t nextId = 1;
    std::map<std::uint32_t, int> live; // driver -> start time

    std::uint32_t startDriver() {
      live[nextId] = now;
      return nextId++;
    }
    void cancel(std::uint32_t id) { live.erase(id); }
    [[nodiscard]] bool alive(std::uint32_t id) const { return live.contains(id); }
  };

  enum class Kind : std::uint8_t { Want, Retarget, Started, Tick, Jump };
  struct Event {
    Kind kind;
    int arg; // Want: face; Started: generations back; Tick: ms
    bool rectChanged = true;
  };

  std::string describe(const Event& e) {
    static const char* const faces[] = {"Rest", "Osd", "Banner"};
    switch (e.kind) {
    case Kind::Want: return std::string("Want{") + faces[e.arg] + (e.rectChanged ? ",newRect}" : ",sameRect}");
    case Kind::Retarget: return "Retarget{}";
    case Kind::Started: return "Started{gen-" + std::to_string(e.arg) + "}";
    case Kind::Tick: return "Tick{" + std::to_string(e.arg) + "ms}";
    case Kind::Jump: return "Jump{}";
    }
    return "?";
  }

  struct Rect {
    int id = 0;
  };

  class MorphSim {
  public:
    using Morph = IslandMorph<Face, Rect, std::uint32_t>;

    void want(Face face, bool rectChanged) {
      if (face == m.shown()) {
        return;
      }
      fadeHeld = true;
      reveal = 0.0F;
      const auto r = m.apply(Morph::Begin{face, Rect{}, env.startDriver()});
      TEST_CHECK(r.accepted);
      env.cancel(r.cancel);
      if (rectChanged) {
        request();
      } else {
        startClocks();
      }
    }
    void retarget() {
      if (m.settled()) {
        fadeHeld = false;
        reveal = 1.0F;
      }
      const std::uint32_t driver = m.settled() ? env.startDriver() : m.driver();
      TEST_CHECK(m.apply(Morph::Retarget{driver}).accepted);
      requested = env.now;
    }
    void started(int back) {
      if (m.apply(Morph::Started{m.gen() - static_cast<std::uint32_t>(back)}).accepted) {
        startClocks();
      }
    }
    void jump() {
      const auto r = m.apply(Morph::Jump{});
      env.cancel(r.cancel);
      reveal = 1.0F;
    }
    void tick(int ms) {
      env.now += ms;
      const std::uint32_t driver = m.driver();
      if (!env.alive(driver)) {
        return;
      }
      step();
      if (env.alive(driver) && env.now - env.live[driver] >= kDriverMaxMs) {
        env.live.erase(driver);
        if (m.apply(Morph::DriverDone{driver}).accepted) {
          reveal = 1.0F;
        }
      }
    }

    [[nodiscard]] Face shown() const { return m.shown(); }
    [[nodiscard]] Face from() const { return m.from(); }
    [[nodiscard]] bool settled() const { return m.settled(); }
    [[nodiscard]] std::uint32_t driver() const { return m.driver(); }
    [[nodiscard]] bool driverLive() const { return env.alive(m.driver()); }
    [[nodiscard]] float revealT() const { return m.settled() ? 1.0F : reveal; }

  private:
    void request() {
      TEST_CHECK(m.apply(Morph::Retarget{m.driver()}).accepted);
      requested = env.now;
    }
    void startClocks() {
      startMs = env.now;
      fadeMs = env.now;
      fadeHeld = false;
    }
    // stepMirror
    void step() {
      if (m.phase() == Morph::Phase::AwaitStart) {
        if (env.now - requested <= kStartWaitMs) {
          return;
        }
        TEST_CHECK(m.apply(Morph::StartTimeout{}).accepted);
        startClocks();
      }
      const bool fadeDone = fadeHeld ? false : reveal >= 1.0F || env.now - fadeMs >= kSettleMs;
      if (!fadeHeld && reveal < 1.0F) {
        reveal = std::min(1.0F, static_cast<float>(env.now - fadeMs) / kSettleMs);
      }
      if (env.now - startMs >= kSettleMs && fadeDone) {
        env.cancel(m.apply(Morph::Settle{}).cancel);
        reveal = 1.0F;
      }
    }

    Env env;
    Morph m{Face::Rest};
    int requested = 0;
    int startMs = 0;
    int fadeMs = 0;
    bool fadeHeld = false;
    float reveal = 1.0F;
  };

  template <typename Sim> void play(Sim& sim, const Event& e) {
    switch (e.kind) {
    case Kind::Want: sim.want(static_cast<Face>(e.arg), e.rectChanged); break;
    case Kind::Retarget: sim.retarget(); break;
    case Kind::Started: sim.started(e.arg); break;
    case Kind::Tick: sim.tick(e.arg); break;
    case Kind::Jump: sim.jump(); break;
    }
  }

  // The outgoing face fades out over the first quarter of the reveal (applyReveal).
  template <typename Sim> bool osdDrawn(const Sim& sim) {
    return sim.shown() == Face::Osd || (sim.from() == Face::Osd && sim.revealT() < 0.25F);
  }

  template <typename Sim> std::optional<std::string> violation(const Sim& sim) {
    if (osdDrawn(sim) && sim.shown() != Face::Osd && (sim.settled() || !sim.driverLive())) {
      return "Osd drawn without a live morph from Osd";
    }
    if (sim.settled() && (sim.driver() != 0 || sim.from() != sim.shown())) {
      return "Settled with a driver or an outgoing face";
    }
    if (!sim.settled() && !sim.driverLive()) {
      return "morph without a live driver";
    }
    return std::nullopt;
  }

  template <typename Sim> std::optional<std::string> check(const std::vector<Event>& events) {
    Sim sim;
    for (const auto& e : events) {
      play(sim, e);
      if (auto v = violation(sim)) {
        return v;
      }
    }
    for (int i = 0; i < 1000 && !sim.settled(); ++i) {
      sim.tick(16);
      if (auto v = violation(sim)) {
        return v;
      }
    }
    if (!sim.settled()) {
      return std::string("never settles");
    }
    return std::nullopt;
  }

  std::vector<Event> randomSequence(std::mt19937& rng) {
    std::uniform_int_distribution<int> length(1, 10);
    std::uniform_int_distribution<int> kind(0, 9);
    std::uniform_int_distribution<int> face(0, kFaces - 1);
    std::uniform_int_distribution<int> back(0, 2);
    std::uniform_int_distribution<int> coin(0, 3);
    const int ticks[] = {1, 16, 100, 450, 600, 3990, 4100, 9000};
    std::uniform_int_distribution<int> tick(0, 7);
    std::vector<Event> events(static_cast<std::size_t>(length(rng)));
    for (auto& e : events) {
      switch (kind(rng)) {
      case 0:
      case 1:
      case 2: e = {Kind::Want, face(rng), coin(rng) != 0}; break;
      case 3: e = {Kind::Retarget, 0}; break;
      case 4:
      case 5: e = {Kind::Started, back(rng)}; break;
      case 9: e = {Kind::Jump, 0}; break;
      default: e = {Kind::Tick, ticks[tick(rng)]}; break;
      }
    }
    return events;
  }

  // Shortest failing sequence over `seeds` random runs; empty when every run holds the invariants.
  template <typename Sim> std::optional<std::pair<std::vector<Event>, std::string>> fuzz(unsigned seeds) {
    std::optional<std::pair<std::vector<Event>, std::string>> shortest;
    for (unsigned seed = 0; seed < seeds; ++seed) {
      std::mt19937 rng(seed);
      const auto events = randomSequence(rng);
      if (auto v = check<Sim>(events); v && (!shortest || events.size() < shortest->first.size())) {
        shortest.emplace(events, *v);
      }
    }
    if (shortest) {
      std::fprintf(stderr, "%s after:", shortest->second.c_str());
      for (const auto& e : shortest->first) {
        std::fprintf(stderr, " %s", describe(e).c_str());
      }
      std::fprintf(stderr, "\n");
    }
    return shortest;
  }

  // The brightness OSD hides, then frames stall past the driver's 4 s cap before the compositor's `started` arrives.
  void osdHideThroughStallSettles() {
    const std::vector<Event> events{{Kind::Want, 1}, {Kind::Want, 0}, {Kind::Tick, 4100}};
    TEST_CHECK(!check<MorphSim>(events).has_value());
  }

  void staleStartedIsRejected() {
    using Morph = MorphSim::Morph;
    Morph m{Face::Rest};
    TEST_CHECK(m.apply(Morph::Begin{Face::Osd, Rect{}, 7}).accepted);
    TEST_CHECK(m.apply(Morph::Retarget{7}).accepted);
    const std::uint32_t first = m.gen();
    TEST_CHECK(m.apply(Morph::Retarget{7}).accepted);
    TEST_CHECK(!m.apply(Morph::Started{first}).accepted);
    TEST_CHECK(!m.apply(Morph::Retarget{8}).accepted);
    TEST_CHECK(!m.apply(Morph::DriverDone{8}).accepted);
    TEST_CHECK(m.apply(Morph::Started{m.gen()}).accepted);
    const auto settled = m.apply(Morph::Settle{});
    TEST_CHECK(settled.cancel == 7 && m.settled() && m.from() == Face::Osd && m.driver() == 0);
  }

} // namespace

int main() {
  osdHideThroughStallSettles();
  staleStartedIsRejected();
  TEST_CHECK(!fuzz<MorphSim>(5000).has_value());
  return 0;
}
