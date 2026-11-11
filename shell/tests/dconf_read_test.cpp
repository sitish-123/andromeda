#include "system/dconf_read.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

  constexpr std::string_view kIconThemeKey = "/org/gnome/desktop/interface/icon-theme";

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "dconf_read_test: {}", message);
    }
    return condition;
  }

  void writeFile(const fs::path& path, std::string_view data) {
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

} // namespace

int main() {
  if (std::system("command -v dconf >/dev/null 2>&1") != 0) {
    std::println("dconf_read_test: dconf binary not found, skipping");
    return 77;
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() / ("noctalia-dconf-read-test-" + std::to_string(now));
  const fs::path keyfiles = dir / "keyfiles";
  fs::create_directories(keyfiles);
  writeFile(
      keyfiles / "00-test",
      "[org/gnome/desktop/interface]\nicon-theme='Papirus-Dark'\ncursor-size=24\n\n[org/example]\nname='other'\n"
  );
  const fs::path database = dir / "user";
  const std::string command = "dconf compile '" + database.string() + "' '" + keyfiles.string() + "'";
  if (std::system(command.c_str()) != 0) {
    std::println(stderr, "dconf_read_test: dconf compile failed");
    fs::remove_all(dir);
    return 1;
  }

  bool ok = true;
  const auto theme = dconf::readStringFromDatabase(database, kIconThemeKey);
  ok &= expect(theme == "Papirus-Dark", "icon-theme should read back as Papirus-Dark");
  ok &= expect(dconf::readStringFromDatabase(database, "/org/example/name") == "other", "second key should read");
  ok &= expect(!dconf::readStringFromDatabase(database, "/org/gnome/desktop/interface/missing"), "missing key");
  ok &= expect(!dconf::readStringFromDatabase(database, "/org/gnome/desktop/interface/icon"), "key prefix");
  ok &= expect(!dconf::readStringFromDatabase(database, "/org/gnome/desktop/interface/cursor-size"), "non-string");
  ok &= expect(!dconf::readStringFromDatabase(dir / "absent", kIconThemeKey), "absent file");

  std::string bytes;
  {
    std::ifstream file(database, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    writeFile(dir / "truncated", std::string_view(bytes).substr(0, size));
    const auto value = dconf::readStringFromDatabase(dir / "truncated", kIconThemeKey);
    ok &= expect(!value.has_value() || value == "Papirus-Dark", "truncated file must not return a wrong value");
  }
  writeFile(dir / "header-only", std::string_view(bytes).substr(0, 24));
  ok &= expect(!dconf::readStringFromDatabase(dir / "header-only", kIconThemeKey), "header-only file");

  std::string garbage = bytes;
  for (std::size_t i = 8; i < garbage.size(); ++i) {
    garbage[i] = static_cast<char>(0xff);
  }
  writeFile(dir / "garbage", garbage);
  ok &= expect(!dconf::readStringFromDatabase(dir / "garbage", kIconThemeKey), "garbage file");
  writeFile(dir / "text", "not a gvdb file at all, just text padding the header");
  ok &= expect(!dconf::readStringFromDatabase(dir / "text", kIconThemeKey), "text file");

  // Every item is an empty key segment whose parent is itself: a cycle that consumes no key bytes.
  constexpr std::uint32_t kCycleItems = 100000;
  std::string cyclic = "GVariant";
  const auto appendU32 = [&cyclic](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      cyclic.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
  };
  appendU32(0);
  appendU32(0);
  appendU32(24);
  appendU32(24 + 8 + (kCycleItems * 24));
  appendU32(0);
  appendU32(0);
  for (std::uint32_t i = 0; i < kCycleItems; ++i) {
    appendU32(0);
    appendU32(i);
    appendU32(0);
    cyclic += std::string_view("\0\0v\0", 4);
    appendU32(0);
    appendU32(0);
  }
  writeFile(dir / "cyclic", cyclic);
  const auto cycleStart = std::chrono::steady_clock::now();
  ok &= expect(!dconf::readStringFromDatabase(dir / "cyclic", kIconThemeKey), "cyclic file");
  ok &= expect(std::chrono::steady_clock::now() - cycleStart < std::chrono::seconds(2), "cyclic file must read fast");

  fs::remove_all(dir);
  return ok ? 0 : 1;
}
