#include "dbus/systemd/systemd_unit_name.h"
#include "tests/test_check.h"

int main() {
  // Names seen on a live session (2026-11-04) and in systemd.io/DESKTOP_ENVIRONMENTS.
  TEST_CHECK(appIdFromUnit("app-zen\\x2dbrowser@2c01d14fe47d4753b986d4be228a8f7f.service") == "zen-browser");
  TEST_CHECK(appIdFromUnit("app-com.sampurna.ai.news@2c01d14f.service") == "com.sampurna.ai.news");
  TEST_CHECK(appIdFromUnit("app-limine\\x2dsnapper\\x2dnotify@autostart.service") == "limine-snapper-notify");
  TEST_CHECK(appIdFromUnit("app-flatpak-org.telegram.desktop@12345.service") == "org.telegram.desktop");
  TEST_CHECK(appIdFromUnit("app-flatpak-org.telegram.desktop-12345.scope") == "org.telegram.desktop");
  TEST_CHECK(appIdFromUnit("app-gnome-org.gnome.Nautilus-4242.scope") == "org.gnome.Nautilus");
  TEST_CHECK(appIdFromUnit("flatpak-org.mozilla.firefox-777.scope") == "org.mozilla.firefox");
  TEST_CHECK(appIdFromUnit("dbus-:1.2-org.kde.kded6@0.service") == "org.kde.kded6");
  // A launcher's PID in place of the AppID carries no app.
  TEST_CHECK(appIdFromUnit("app-andromeda-1835-17.scope").empty());
  TEST_CHECK(appIdFromUnit("opencode.service").empty());
  TEST_CHECK(appIdFromUnit("init.scope").empty());

  TEST_CHECK(escapeUnitName("limine-snapper-notify") == "limine\\x2dsnapper\\x2dnotify");
  TEST_CHECK(escapeUnitName(".hidden") == "\\x2ehidden");
  TEST_CHECK(unescapeUnitName(escapeUnitName("a b-c")) == "a b-c");

  TEST_CHECK(classifySlice("app.slice") == UnitSlice::App);
  TEST_CHECK(classifySlice("app-dbus\\x2d:1.2\\x2dorg.kde.kded6.slice") == UnitSlice::App);
  TEST_CHECK(classifySlice("background.slice") == UnitSlice::Background);
  TEST_CHECK(classifySlice("session.slice") == UnitSlice::Session);
  TEST_CHECK(classifySlice("user-1000.slice") == UnitSlice::Other);
  return 0;
}
