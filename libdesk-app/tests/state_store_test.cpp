#include "app/state_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

namespace {
  int g_failures = 0;

  void expect(bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "state_store_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  char tmpl[] = "/tmp/desk-app-state-XXXXXX";
  const std::filesystem::path dir = ::mkdtemp(tmpl);
  ::setenv("XDG_STATE_HOME", dir.c_str(), 1);
  const std::filesystem::path file = StateStore::pathFor("dev.andromeda.Test");
  expect(file == dir / "andromeda/dev.andromeda.Test/state.json", "path is under XDG_STATE_HOME/andromeda/<app-id>");

  {
    StateStore store(file);
    store.load();
    expect(store.data().empty(), "a missing file loads as empty state");
    store.set("/window/main/width", 812);
    store.set("/window/main/maximized", true);
    expect(store.dirty(), "a change marks the store dirty");
    expect(store.flush(), "flush writes the file");
    expect(!std::filesystem::exists(file.string() + ".tmp"), "no temp file stays after a write");
  }
  {
    StateStore store(file);
    store.load();
    expect(store.get<int>("/window/main/width", 0) == 812, "a saved value loads back");
    expect(store.get<bool>("/window/main/maximized", false), "a saved bool loads back");
    expect(store.get<int>("/window/other/width", 7) == 7, "an absent key gives the fallback");
    expect(store.get<std::string>("/window/main/width", "x") == "x", "a value of another type gives the fallback");
    store.set("/window/main/width", 812);
    expect(!store.dirty(), "setting the same value is no change");
  }
  {
    // The destructor writes a pending change, as when the window closes before the save delay.
    StateStore store(file);
    store.load();
    store.set("/sample/clicks", 3);
  }
  {
    StateStore store(file);
    store.load();
    expect(store.get<int>("/sample/clicks", 0) == 3, "a pending change is written on destruction");
  }
  {
    std::ofstream(file) << "{ not json";
    StateStore store(file);
    store.load();
    expect(store.data().empty(), "a corrupt file loads as empty state");
    expect(std::filesystem::exists(file.string() + ".bad"), "a corrupt file is kept as .bad");
  }

  std::filesystem::remove_all(dir);
  return g_failures == 0 ? 0 : 1;
}
