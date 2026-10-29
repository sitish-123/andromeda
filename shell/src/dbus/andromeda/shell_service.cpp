#include "dbus/andromeda/shell_service.h"

#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <vector>

namespace {

  const sdbus::ServiceName kBusName{"org.andromeda.Shell1"};
  const sdbus::ObjectPath kObjectPath{"/org/andromeda/Shell1"};
  constexpr auto kInterface = "org.andromeda.Shell1";

} // namespace

AndromedaShellService::AndromedaShellService(
    SessionBus& bus, NotificationManager& notifications, std::function<bool()> screenOff
)
    : m_notifications(notifications), m_screenOff(std::move(screenOff)), m_doNotDisturb(notifications.doNotDisturb()) {
  bus.connection().requestName(kBusName);
  m_object = sdbus::createObject(bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerProperty("DoNotDisturb")
              .withGetter([this] { return m_notifications.doNotDisturb(); })
              .withSetter([this](const bool& on) { m_notifications.setDoNotDisturb(on); }),
          sdbus::registerMethod("ScreenOff").implementedAs([this] {
            if (!m_screenOff()) {
              throw sdbus::Error(sdbus::Error::Name{"org.andromeda.Shell1.Error.Failed"}, "the outputs stayed on");
            }
          })
      )
      .forInterface(kInterface);
}

void AndromedaShellService::notificationStateChanged() {
  const bool now = m_notifications.doNotDisturb();
  if (now == m_doNotDisturb) {
    return;
  }
  m_doNotDisturb = now;
  m_object->emitPropertiesChangedSignal(
      kInterface, std::vector<sdbus::PropertyName>{sdbus::PropertyName{"DoNotDisturb"}}
  );
}
