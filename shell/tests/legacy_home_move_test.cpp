#include "config/legacy_home_move.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;
  using noctalia::config::LegacyHomeMove;
  using noctalia::config::moveLegacyHome;

  fs::path makeTempRoot(const char* label) {
    auto path = fs::temp_directory_path() / (std::string("legacy-home-") + label + "-" + std::to_string(getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
  }

  void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "legacy_home_move_test: {}", message);
    }
    return condition;
  }

  bool movesLegacyOnly() {
    const fs::path root = makeTempRoot("old-only");
    writeFile(root / "noctalia" / "config.toml", "[bar]\n");
    bool ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Moved, "old only: moved");
    ok = expect(readFile(root / "andromeda" / "shell" / "config.toml") == "[bar]\n", "old only: file at the new path") && ok;
    ok = expect(fs::is_symlink(root / "noctalia"), "old only: old path is a link") && ok;
    ok = expect(readFile(root / "noctalia" / "config.toml") == "[bar]\n", "old only: old path reaches the new dir") && ok;
    ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Nothing, "second run: nothing") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool leavesNewOnly() {
    const fs::path root = makeTempRoot("new-only");
    writeFile(root / "andromeda" / "shell" / "config.toml", "new\n");
    bool ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Nothing, "new only: nothing");
    ok = expect(!fs::exists(fs::symlink_status(root / "noctalia")), "new only: no old link") && ok;
    ok = expect(readFile(root / "andromeda" / "shell" / "config.toml") == "new\n", "new only: new file kept") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool keepsNewWhenBoth() {
    const fs::path root = makeTempRoot("both");
    writeFile(root / "noctalia" / "config.toml", "old\n");
    writeFile(root / "andromeda" / "shell" / "config.toml", "new\n");
    bool ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::KeptTarget, "both: kept new");
    ok = expect(readFile(root / "andromeda" / "shell" / "config.toml") == "new\n", "both: new file kept") && ok;
    ok = expect(!fs::is_symlink(root / "noctalia") && readFile(root / "noctalia" / "config.toml") == "old\n", "both: old dir untouched") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool ignoresNeither() {
    const fs::path root = makeTempRoot("neither");
    bool ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Nothing, "neither: nothing");
    ok = expect(!fs::exists(root / "andromeda") && !fs::exists(fs::symlink_status(root / "noctalia")), "neither: creates nothing") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool movesLegacyLink() {
    const fs::path root = makeTempRoot("link");
    writeFile(root / "elsewhere" / "config.toml", "linked\n");
    fs::create_directory_symlink("elsewhere", root / "noctalia");
    bool ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Moved, "link: moved");
    ok = expect(fs::is_symlink(root / "andromeda" / "shell"), "link: new path is the link") && ok;
    ok = expect(readFile(root / "andromeda" / "shell" / "config.toml") == "linked\n", "link: new path reaches the dir") && ok;
    ok = expect(readFile(root / "noctalia" / "config.toml") == "linked\n", "link: old path still reaches the dir") && ok;
    ok = expect(moveLegacyHome(root / "noctalia", root / "andromeda" / "shell") == LegacyHomeMove::Nothing, "link: second run nothing") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool dropsDisposableLegacyWhenBoth() {
    const fs::path root = makeTempRoot("cache-both");
    writeFile(root / "noctalia" / "thumb", "old\n");
    writeFile(root / "andromeda" / "shell" / "thumb", "new\n");
    bool ok = expect(
        moveLegacyHome(root / "noctalia", root / "andromeda" / "shell", noctalia::config::LegacyContent::Disposable)
            == LegacyHomeMove::DroppedLegacy,
        "cache both: dropped old"
    );
    ok = expect(fs::is_symlink(root / "noctalia"), "cache both: old path is a link") && ok;
    ok = expect(readFile(root / "noctalia" / "thumb") == "new\n", "cache both: old path reaches the new dir") && ok;
    fs::remove_all(root);
    return ok;
  }

  bool movesEveryRootUnderOverrides() {
    const fs::path root = makeTempRoot("roots");
    ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("ANDROMEDA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("ANDROMEDA_DATA_HOME", (root / "data").c_str(), 1);
    ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
    for (const char* base : {"config", "state", "data", "cache"}) {
      writeFile(root / base / "noctalia" / "file", base);
    }
    noctalia::config::moveLegacyHomes();
    bool ok = true;
    for (const char* base : {"config", "state", "data", "cache"}) {
      ok = expect(readFile(root / base / "andromeda" / "shell" / "file") == base, "roots: file at the new path") && ok;
      ok = expect(fs::is_symlink(root / base / "noctalia"), "roots: old path is a link") && ok;
    }
    fs::remove_all(root);
    return ok;
  }

} // namespace

int main() {
  bool ok = movesLegacyOnly();
  ok = leavesNewOnly() && ok;
  ok = keepsNewWhenBoth() && ok;
  ok = ignoresNeither() && ok;
  ok = movesLegacyLink() && ok;
  ok = dropsDisposableLegacyWhenBoth() && ok;
  ok = movesEveryRootUnderOverrides() && ok;
  return ok ? 0 : 1;
}
