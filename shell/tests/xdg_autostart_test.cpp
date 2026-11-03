#include "system/xdg_autostart.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

  void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  const AutostartEntry* find(const std::vector<AutostartEntry>& entries, const std::string& id) {
    const auto it = std::ranges::find(entries, id, &AutostartEntry::id);
    return it == entries.end() ? nullptr : &*it;
  }

} // namespace

int main() {
  char tmpl[] = "/tmp/xdg-autostart-test-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const AutostartDirs dirs{.user = root / "user", .system = {root / "etc1", root / "etc2"}};

  write(root / "etc1/syncthing.desktop", "[Desktop Entry]\nType=Application\nName=Syncthing\nExec=syncthing\n");
  // A user file with Hidden=true overrides the system one of the same name.
  write(root / "etc1/blueman.desktop", "[Desktop Entry]\nName=Blueman Applet\nExec=blueman-applet\n");
  write(root / "user/blueman.desktop", "[Desktop Entry]\nName=Blueman Applet\nHidden=true\n");
  // The earlier system directory wins over a later one.
  write(root / "etc2/syncthing.desktop", "[Desktop Entry]\nName=Other\nExec=other\n");
  write(root / "etc2/kde-only.desktop", "[Desktop Entry]\nName=KDE\nExec=kded\nOnlyShowIn=KDE;\n");
  write(root / "etc2/not-here.desktop", "[Desktop Entry]\nName=N\nExec=n\nNotShowIn=GNOME;Andromeda;\n");
  write(root / "etc2/skip.desktop", "[Desktop Entry]\nName=S\nExec=s\nX-systemd-skip=true\n");
  write(root / "etc2/gnome-off.desktop", "[Desktop Entry]\nName=G\nExec=g\nX-GNOME-Autostart-enabled=false\n");
  write(root / "etc2/missing-tryexec.desktop", "[Desktop Entry]\nName=T\nExec=t\nTryExec=/nonexistent/bin\n");
  write(root / "etc2/limine-snapper-notify.desktop", "[Desktop Entry]\nName=Limine\nExec=l\nTryExec=/bin/sh\n");
  write(root / "etc2/notes.txt", "not a desktop file");

  const auto entries = scanAutostart(dirs, "Andromeda:wlroots");
  TEST_CHECK(entries.size() == 8); // syncthing in etc2 is shadowed by etc1

  const auto* syncthing = find(entries, "syncthing");
  TEST_CHECK(syncthing != nullptr && syncthing->name == "Syncthing" && syncthing->startsHere && !syncthing->userFile);

  const auto* blueman = find(entries, "blueman");
  TEST_CHECK(blueman != nullptr && blueman->userFile && !blueman->startsHere);

  TEST_CHECK(!find(entries, "kde-only")->startsHere);
  TEST_CHECK(!find(entries, "not-here")->startsHere);
  TEST_CHECK(!find(entries, "skip")->startsHere);
  TEST_CHECK(!find(entries, "gnome-off")->startsHere);
  TEST_CHECK(!find(entries, "missing-tryexec")->startsHere);

  const auto* limine = find(entries, "limine-snapper-notify");
  TEST_CHECK(limine->startsHere);
  TEST_CHECK(limine->unitName == "app-limine\\x2dsnapper\\x2dnotify@autostart.service");

  fs::remove_all(root);
  return 0;
}
