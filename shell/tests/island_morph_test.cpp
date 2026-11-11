#include "shell/live_activity/island_morph_driver.h"
#include "tests/test_check.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Drives the island morph and its driver glue through random event sequences: drivers are fake AnimationManager
// timers (4 s cap, a stalled frame jumps past it), and the compositor sends `started` late, stale or never.
namespace {

  enum class Face : std::uint8_t { Rest, Osd, Banner };
  constexpr int kFaces = 3;
  constexpr std::uint32_t kDriverMaxMs = 4000; // kMirrorMaxMs

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

  constexpr desk::motion::SpringConfig kSpring{.damping = 0.9, .stiffness = 300.0};

  IslandMorphDriver<Face, Rect, std::uint32_t>::Shape shapeOf(Face face) {
    switch (face) {
    case Face::Rest: return {0.0, 0.0, 40.0, 10.0, 5.0};
    case Face::Osd: return {0.0, 0.0, 200.0, 50.0, 25.0};
    case Face::Banner: return {0.0, 0.0, 300.0, 100.0, 25.0};
    }
    return {};
  }

  // AnimationManager timers capped at kDriverMaxMs, the 1 ms reaper timer, the clock and the island's fade.
  struct Host {
    std::uint32_t now = 1;
    std::uint32_t nextId = 1;
    std::map<std::uint32_t, std::uint32_t> live; // driver -> start time
    bool reapPending = false;
    float revealed = 1.0F;

    [[nodiscard]] std::uint32_t nowMs() const { return now; }
    std::uint32_t startDriver() {
      live[nextId] = now;
      return nextId++;
    }
    void cancel(std::uint32_t id) { live.erase(id); }
    void reapSoon() {
      TEST_CHECK(!reapPending);
      reapPending = true;
    }
    void redraw() {}
    void reveal(float t) { revealed = t; }
    void settle() { revealed = 1.0F; }
  };

  // The call order of LiveActivityIsland's mirrored path (animateReveal, syncDsk, the timer's tick and onComplete)
  // around the real IslandMorphDriver.
  class Island {
  public:
    using Driver = IslandMorphDriver<Face, Rect, std::uint32_t>;
    using Morph = Driver::Morph;

    void want(Face face, bool rectChanged) {
      if (face == d.shown()) {
        return;
      }
      host.revealed = 0.0F;
      const auto begun = d.begin(host, face, Rect{static_cast<int>(d.shown())}, kSpring);
      TEST_CHECK(begun.accepted && d.fromRect().id == static_cast<int>(d.from()));
      if (rectChanged) {
        sync(begun.wasMoving);
      } else {
        d.startShape(host, current(), shapeOf(face), kSpring, begun.wasMoving);
        d.startFadeNow();
      }
    }
    // A content width change while shown.
    void retarget() { sync(!d.settled()); }
    void started(int back) {
      const std::uint32_t gen = d.gen() - static_cast<std::uint32_t>(back);
      d.started(host, gen, host.now, continued[gen]);
    }
    void jump() {
      d.jump(host);
      host.revealed = 1.0F;
    }
    // A stalled frame jumps past a driver's cap: it ticks once more, then ends on its own.
    void tick(int ms) {
      host.now += static_cast<std::uint32_t>(ms);
      if (host.reapPending) {
        host.reapPending = false;
        d.reap(host);
      }
      std::vector<std::uint32_t> ids;
      for (const auto& [id, start] : host.live) {
        ids.push_back(id);
      }
      for (const std::uint32_t id : ids) {
        if (!host.live.contains(id)) {
          continue;
        }
        d.step(host);
        if (host.live.contains(id) && host.now - host.live[id] >= kDriverMaxMs) {
          host.live.erase(id);
          d.driverDone(host, id);
        }
      }
    }

    [[nodiscard]] Face shown() const { return d.shown(); }
    [[nodiscard]] Face from() const { return d.from(); }
    [[nodiscard]] bool settled() const { return d.settled(); }
    [[nodiscard]] Morph::Phase phase() const { return d.phase(); }
    [[nodiscard]] std::uint32_t driver() const { return d.driver(); }
    [[nodiscard]] bool driverLive() const { return host.live.contains(d.driver()); }
    [[nodiscard]] bool anyDriverLive() const { return !host.live.empty(); }
    [[nodiscard]] float revealT() const { return d.settled() ? 1.0F : host.revealed; }

  private:
    // syncDsk, animated: the compositor gets the new clip and answers with `started` for this generation.
    void sync(bool continueFrom) {
      d.startShape(host, current(), shapeOf(d.shown()), kSpring, continueFrom);
      continued[d.retarget(host, kSpring)] = continueFrom;
      TEST_CHECK(d.phase() == Morph::Phase::AwaitStart && driverLive());
    }
    [[nodiscard]] Driver::Shape current() const {
      Driver::Shape s{};
      for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = d.shape()[i].value();
      }
      return s;
    }

    Host host;
    Driver d{Face::Rest};
    std::map<std::uint32_t, bool> continued; // gen -> the retarget continued a running morph
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
    sim.tick(16);
    if (sim.anyDriverLive()) {
      return std::string("a driver outlives its morph");
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
    TEST_CHECK(!check<Island>(events).has_value());
  }

  // The springs end a morph long before the driver cap ends it.
  void springsSettleBeforeCap() {
    Island sim;
    sim.want(Face::Osd, false);
    std::uint32_t ms = 0;
    while (!sim.settled() && ms < kDriverMaxMs) {
      sim.tick(16);
      ms += 16;
    }
    TEST_CHECK(sim.settled() && ms < 2000);
  }

  // With no `started`, the morph waits kStartWaitMs, then runs on its own.
  void startWaitTimesOut() {
    using Morph = Island::Morph;
    Island sim;
    sim.want(Face::Osd, true);
    sim.tick(Island::Driver::kStartWaitMs);
    TEST_CHECK(sim.phase() == Morph::Phase::AwaitStart);
    sim.tick(16);
    TEST_CHECK(!sim.settled() && sim.phase() != Morph::Phase::AwaitStart);
  }

  void staleStartedIsRejected() {
    using Morph = Island::Morph;
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
  springsSettleBeforeCap();
  startWaitTimesOut();
  TEST_CHECK(!fuzz<Island>(5000).has_value());
  return 0;
}
