#include "check.h"
#include "config/config.h"
#include "server/xdg_session_state.h"

#include <array>
#include <nlohmann/json.hpp>
#include <random>

using namespace andromeda;

namespace {

  SessionPlacement at(std::string output, size_t position) {
    return {.output = std::move(output), .workspace = WorkspaceIndex{position}, .floating = true, .width = 640, .height = 480};
  }

} // namespace

ANDROMEDA_TEST(aMalformedStateFileKeepsItsGoodEntriesAndSkipsTheRest) {
  const nlohmann::json file = nlohmann::json::parse(R"({
    "version": 2,
    "sessions": {
      "good": {"used": 3, "toplevels": {
        "main": {"output": "HEADLESS-1", "workspace": 2, "floating": true, "w": 640, "h": 480},
        "size": {"w": "x"},
        "output": {"output": 1},
        "flag": {"floating": "yes"},
        "workspace": {"workspace": [1]},
        "zero": {"workspace": 0},
        "scalar": 5
      }},
      "notAnObject": 7,
      "noToplevels": {"used": 1},
      "badUsed": {"used": "x", "toplevels": {"main": {}}},
      "listToplevels": {"toplevels": [1, 2]}
    }
  })");
  size_t skipped = 0;
  const SessionStore store = SessionStore::fromJson(file, skipped);
  CHECK_EQ(skipped, size_t{10});
  CHECK_EQ(store.size(), size_t{1});
  const SessionPlacement* main = store.find("good", "main");
  CHECK(main != nullptr);
  if (main != nullptr) {
    CHECK(*main == at("HEADLESS-1", 2));
  }
  CHECK(store.find("good", "size") == nullptr);
}

ANDROMEDA_TEST(aStateFileOfAnotherShapeOrVersionLoadsNothing) {
  for (const char* text : {R"([1])", R"({"version": 1, "sessions": {}})", R"({"version": 2, "sessions": []})",
                           R"({"version": "2", "sessions": {}})", R"("text")"}) {
    size_t skipped = 0;
    const SessionStore store = SessionStore::fromJson(nlohmann::json::parse(text), skipped);
    CHECK_EQ(store.size(), size_t{0});
    CHECK_EQ(skipped, size_t{1});
  }
  size_t skipped = 0;
  CHECK_EQ(SessionStore::fromJson(nlohmann::json::parse("{", nullptr, false), skipped).size(), size_t{0});
}

ANDROMEDA_TEST(theStoreSurvivesItsOwnFile) {
  SessionStore store;
  CHECK(store.put("a", "main", at("DP-1", 3)));
  CHECK(!store.put("a", "main", at("DP-1", 3)));
  CHECK(store.put("b", "x", {.output = "DP-2", .workspace = WorkspaceName{"mail"}, .fullscreen = true}));
  size_t skipped = 0;
  const SessionStore loaded = SessionStore::fromJson(nlohmann::json::parse(store.toJson().dump()), skipped);
  CHECK_EQ(skipped, size_t{0});
  CHECK_EQ(loaded.toJson(), store.toJson());
}

ANDROMEDA_TEST(theStoreDropsTheLeastRecentlyUsedSessionPastItsCap) {
  SessionStore store;
  for (size_t i = 0; i < SessionStore::kMaxSessions; ++i) {
    CHECK(store.put(std::format("s{}", i), "main", at("DP-1", 1)));
  }
  store.touch("s0");
  CHECK(store.put("new", "main", at("DP-1", 1)));
  CHECK_EQ(store.size(), SessionStore::kMaxSessions);
  CHECK(store.contains("s0"));
  CHECK(!store.contains("s1"));
  CHECK(store.contains("new"));

  // A session cannot grow past kMaxToplevels names; a known name still updates.
  for (size_t i = 1; i < SessionStore::kMaxToplevels; ++i) {
    CHECK(store.put("new", std::format("w{}", i), at("DP-1", 1)));
  }
  CHECK(!store.put("new", "one-too-many", at("DP-1", 1)));
  CHECK(store.put("new", "main", at("DP-1", 2)));

  // A file with more sessions than the cap keeps the most recently used.
  nlohmann::json file = {{"version", 2}, {"sessions", nlohmann::json::object()}};
  for (size_t i = 0; i <= SessionStore::kMaxSessions; ++i) {
    file["sessions"][std::format("f{}", i)] = {{"used", i + 1}, {"toplevels", {{"main", nlohmann::json::object()}}}};
  }
  size_t skipped = 0;
  const SessionStore loaded = SessionStore::fromJson(file, skipped);
  CHECK_EQ(loaded.size(), SessionStore::kMaxSessions);
  CHECK(!loaded.contains("f0"));
  CHECK(loaded.contains("f1"));
}

ANDROMEDA_TEST(aNumericWorkspaceNameRestoresByNameAndAPositionByPosition) {
  const SessionPlacement named{.output = "DP-1", .workspace = WorkspaceName{"2"}};
  const SessionPlacement positioned{.output = "DP-1", .workspace = WorkspaceIndex{2}};
  for (const SessionPlacement& placement : {named, positioned}) {
    const std::optional<SessionPlacement> read = sessionPlacementFrom(sessionPlacementJson(placement));
    CHECK(read.has_value());
    ResolvedWindowRule rule;
    applySessionPlacement(read.value_or(SessionPlacement{}), rule);
    CHECK(rule.defaultWorkspace == placement.workspace);
  }
}

// Random get_session / add / restore / configure / map / destroy toplevel / destroy handle / remove sequences over a
// few names of one session, through transitionSessionEntry.
ANDROMEDA_TEST(randomSessionEventsKeepTheEntryInvariants) {
  using S = SessionEntryState;
  using E = SessionEntryEvent;
  constexpr size_t kNames = 3;
  enum class Op : uint8_t { Get, Add, Restore, Configure, Map, DestroyToplevel, DestroyResource, Remove, Count };
  struct Entry {
    S state = S::Free;
    bool resource = false;
    bool fromStoredRestore = false;
    int restoredSent = 0;
  };
  std::mt19937 random(0x5e55);
  for (int run = 0; run < 2000; ++run) {
    std::array<Entry, kNames> entries{};
    std::array<bool, kNames> stored{};
    for (int i = 0; i < 60; ++i) {
      const auto op = static_cast<Op>(random() % static_cast<unsigned>(Op::Count));
      Entry& entry = entries[random() % kNames];
      const bool isStored = stored[&entry - entries.data()];
      const auto apply = [&](Entry& target, E event) {
        const std::optional<S> next = transitionSessionEntry(target.state, event);
        if (next && sessionEntrySendsRestored(target.state, *next) && target.resource) {
          ++target.restoredSent;
        }
        if (next) {
          target.state = *next;
        }
        return next;
      };
      switch (op) {
      case Op::Get:
        // A new session object: the old one's entries go.
        for (Entry& each : entries) {
          if (each.state != S::Free) {
            CHECK_EQ(apply(each, E::Remove), std::optional(S::Free));
          }
          each = {};
        }
        break;
      case Op::Add:
      case Op::Restore: {
        const bool wasFree = entry.state == S::Free;
        const E event = op == Op::Add ? E::Add : isStored ? E::Restore : E::RestoreUnknown;
        const std::optional<S> next = apply(entry, event);
        // A name is free again once its toplevel died or was removed; otherwise reusing it is name_in_use.
        CHECK_EQ(next.has_value(), wasFree);
        if (next) {
          entry.resource = true;
          entry.fromStoredRestore = event == E::Restore;
          entry.restoredSent = 0;
          CHECK_EQ(*next, entry.fromStoredRestore ? S::Restoring : S::Tracking);
        }
        break;
      }
      case Op::Configure:
      case Op::Map: {
        const bool pending = entry.state == S::Restoring || entry.state == S::Configured;
        const std::optional<S> next = apply(entry, op == Op::Map ? E::Map : E::Configure);
        // Only a restored entry has a placement to apply, and only until its window maps.
        CHECK_EQ(next.has_value(), pending);
        if (op == Op::Map && next) {
          CHECK_EQ(*next, S::Tracking);
        }
        if (next && entry.resource) {
          CHECK_EQ(entry.restoredSent, 1);
        }
        break;
      }
      case Op::DestroyToplevel:
      case Op::Remove: {
        const bool wasFree = entry.state == S::Free;
        const std::optional<S> next = apply(entry, op == Op::Remove ? E::Remove : E::ToplevelDestroyed);
        CHECK_EQ(next.has_value(), !wasFree);
        CHECK_EQ(entry.state, S::Free);
        if (op == Op::Remove) {
          stored[&entry - entries.data()] = false;
        }
        break;
      }
      case Op::DestroyResource: {
        // Destroying the handle leaves window management as it is.
        const S before = entry.state;
        const std::optional<S> next = apply(entry, E::ResourceDestroyed);
        CHECK_EQ(next.has_value(), before != S::Free);
        CHECK_EQ(entry.state, before);
        entry.resource = false;
        break;
      }
      case Op::Count:
        break;
      }
      for (size_t n = 0; n < kNames; ++n) {
        const Entry& each = entries[n];
        CHECK(each.restoredSent <= 1);
        CHECK(each.restoredSent == 0 || each.fromStoredRestore);
        // A tracked entry records its window; a stored name then exists.
        if (each.state == S::Tracking && random() % 4 == 0) {
          stored[n] = true;
        }
      }
    }
  }
}

int main() { return RUN_TESTS(); }
