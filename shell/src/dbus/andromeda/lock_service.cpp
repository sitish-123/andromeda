#include "dbus/andromeda/lock_service.h"

#include "dbus/session_bus.h"

#include <optional>
#include <vector>

namespace {

  const sdbus::ServiceName kBusName{"org.andromeda.Lock1"};
  const sdbus::ObjectPath kObjectPath{"/org/andromeda/Lock1"};
  constexpr auto kInterface = "org.andromeda.Lock1";

} // namespace

LockDBusService::LockDBusService(SessionBus& bus, Handlers handlers) : m_handlers(std::move(handlers)) {
  bus.connection().requestName(kBusName);
  m_object = sdbus::createObject(bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerProperty("Active").withGetter([this] { return m_handlers.active(); }),
          sdbus::registerProperty("Locked").withGetter([this] { return m_handlers.locked(); }),
          sdbus::registerMethod("Lock").withInputParamNames("reason").withOutputParamNames("locking").implementedAs(
              [this](const std::string& reason) { return m_handlers.lock(reason); }
          ),
          sdbus::registerMethod("PrimeCapture").implementedAs([this] { m_handlers.primeCapture(); }),
          sdbus::registerMethod("ClearCapture").implementedAs([this] { m_handlers.clearCapture(); }),
          sdbus::registerMethod("PrepareOutputPowerOff")
              .withOutputParamNames("ready")
              .implementedAs([this](sdbus::Result<bool>&& result) {
                auto shared = std::make_shared<std::optional<sdbus::Result<bool>>>(std::move(result));
                m_handlers.prepareOutputPowerOff([shared](bool ready) {
                  if (shared->has_value()) {
                    (*shared)->returnResults(ready);
                    shared->reset();
                  }
                });
              }),
          sdbus::registerMethod("CancelOutputPowerOff").implementedAs([this] { m_handlers.cancelOutputPowerOff(); }),
          sdbus::registerMethod("OutputsWoke").implementedAs([this] { m_handlers.outputsWoke(); }),
          sdbus::registerMethod("SkipLockOnNextSleep").withInputParamNames("skip").implementedAs([this](bool skip) {
            m_handlers.skipLockOnNextSleep(skip);
          }),
          sdbus::registerSignal("StateChanged").withParameters<std::string>("state"),
          sdbus::registerSignal("FailedAttempts").withParameters<std::vector<std::int64_t>>("rejected_at_ms")
      )
      .forInterface(kInterface);
}

void LockDBusService::stateChanged(const std::string& state) {
  m_object->emitSignal("StateChanged").onInterface(kInterface).withArguments(state);
  m_object->emitPropertiesChangedSignal(
      kInterface, std::vector<sdbus::PropertyName>{sdbus::PropertyName{"Active"}, sdbus::PropertyName{"Locked"}}
  );
}

void LockDBusService::failedAttempts(const std::vector<std::int64_t>& rejectedAtMs) {
  m_object->emitSignal("FailedAttempts").onInterface(kInterface).withArguments(rejectedAtMs);
}
