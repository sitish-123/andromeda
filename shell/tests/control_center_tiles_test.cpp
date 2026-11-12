// Control center toggle tiles show their service's state and a click changes that service.
#include "idle/idle_inhibitor.h"
#include "notification/notification_manager.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/shortcut_services.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <string_view>

namespace {

  void checkToggleTile(
      std::string_view type, const ShortcutServices& services, const std::function<bool()>& serviceOn,
      const std::function<void(bool)>& setServiceOn
  ) {
    const std::unique_ptr<Shortcut> tile = ShortcutRegistry::create(type, services);
    TEST_CHECK(tile != nullptr);
    TEST_CHECK(tile->isToggle());
    TEST_CHECK(tile->iconOn() != tile->iconOff());

    for (const bool on : {true, false, true}) {
      setServiceOn(on);
      TEST_CHECK(tile->active() == on);
      TEST_CHECK(tile->currentIcon() == (on ? tile->iconOn() : tile->iconOff()));
    }

    tile->onClick();
    TEST_CHECK(!serviceOn() && !tile->active());
    tile->onClick();
    TEST_CHECK(serviceOn() && tile->active());
  }

} // namespace

int main() {
  // Plugin shortcuts resolve before built-ins; an empty data dir keeps the user's plugins out.
  ::setenv("ANDROMEDA_DATA_HOME", "/nonexistent/control-center-tiles-test", 1);

  NotificationManager notifications;
  checkToggleTile(
      "notification", ShortcutServices{.notifications = &notifications},
      [&notifications] { return notifications.doNotDisturb(); },
      [&notifications](bool on) { notifications.setDoNotDisturb(on); }
  );

  IdleInhibitor inhibitor;
  checkToggleTile(
      "caffeine", ShortcutServices{.idleInhibitor = &inhibitor}, [&inhibitor] { return inhibitor.enabled(); },
      [&inhibitor](bool on) { inhibitor.setEnabled(on); }
  );

  return 0;
}
