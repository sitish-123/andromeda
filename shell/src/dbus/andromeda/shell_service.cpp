#include "dbus/andromeda/shell_service.h"

#include "dbus/session_bus.h"
#include "core/deferred_call.h"
#include "notification/notification_manager.h"
#include "shell/lockscreen/lock_notice_wire.h"

#include <vector>

namespace {

  const sdbus::ServiceName kBusName{"org.andromeda.Shell1"};
  const sdbus::ObjectPath kObjectPath{"/org/andromeda/Shell1"};
  constexpr auto kInterface = "org.andromeda.Shell1";

} // namespace

AndromedaShellService::AndromedaShellService(
    SessionBus& bus, NotificationManager& notifications, std::function<bool()> screenOff,
    std::function<bool()> lockFromPhone
)
    : m_notifications(notifications), m_screenOff(std::move(screenOff)), m_lockFromPhone(std::move(lockFromPhone)),
      m_doNotDisturb(notifications.doNotDisturb()) {
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
          }),
          // Any session client may call this: it only makes the lock stricter (biometrics and grace off).
          sdbus::registerMethod("LockFromPhone").implementedAs([this] {
            if (!m_lockFromPhone()) {
              throw sdbus::Error(sdbus::Error::Name{"org.andromeda.Shell1.Error.Failed"}, "the session did not lock");
            }
          }),
          sdbus::registerProperty(lock_notice_wire::kProperty).withGetter([this] {
            return lock_notice_wire::encode(m_lockNotices);
          })
      )
      .forInterface(kInterface);
  m_lockNotices = groupUnseenNotices(m_notifications.history());
  // A replace keeps the unread count but can change a group's icon or time, so both hooks feed it.
  m_eventToken = m_notifications.addEventCallback([this](const Notification&, NotificationEvent) {
    scheduleLockNotices();
  });
  m_unreadToken = m_notifications.addUnreadCountCallback([this] { scheduleLockNotices(); });
}

AndromedaShellService::~AndromedaShellService() {
  m_notifications.removeEventCallback(m_eventToken);
  m_notifications.removeUnreadCountCallback(m_unreadToken);
}

void AndromedaShellService::scheduleLockNotices() {
  if (m_lockNoticesPending) {
    return;
  }
  m_lockNoticesPending = true;
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (alive.expired()) {
      return;
    }
    m_lockNoticesPending = false;
    std::vector<LockNoticeGroup> now = groupUnseenNotices(m_notifications.history());
    if (now == m_lockNotices) {
      return;
    }
    m_lockNotices = std::move(now);
    m_object->emitPropertiesChangedSignal(
        kInterface, std::vector<sdbus::PropertyName>{sdbus::PropertyName{lock_notice_wire::kProperty}}
    );
  });
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
