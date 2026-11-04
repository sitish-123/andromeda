#include "dbus/system_bus.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n_service.h"
#include "shell/control_center/tabs/power_tab.h"
#include "tests/test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/toggle.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

  // A fake andromeda-system (org.andromeda.System1): the API the shell reads instead of UPower.
  const sdbus::ServiceName kBusName{"org.andromeda.System1"};
  const sdbus::ObjectPath kRootPath{"/org/andromeda/System1"};
  const sdbus::ObjectPath kPowerPath{"/org/andromeda/System1/Power"};
  constexpr auto kPowerInterface = "org.andromeda.System1.Power";
  constexpr auto kDeviceInterface = "org.andromeda.System1.Battery";
  using ChargeLimit = sdbus::Struct<bool, bool, std::uint32_t, std::uint32_t, std::uint32_t>;

  class FakeBattery {
  public:
    FakeBattery(
        sdbus::IConnection& connection, std::string path, std::string nativePath, std::string model, bool powerSupply
    )
        : connection(connection), path(std::move(path)), nativePath(std::move(nativePath)), model(std::move(model)),
          powerSupply(powerSupply) {
      registerObject();
    }

    void registerObject() {
      object = sdbus::createObject(connection, sdbus::ObjectPath{path});
      object
          ->addVTable(
              sdbus::registerProperty("NativePath").withGetter([this]() { return this->nativePath; }),
              sdbus::registerProperty("Vendor").withGetter([]() { return std::string{"Noctalia Test"}; }),
              sdbus::registerProperty("Model").withGetter([this]() { return this->model; }),
              sdbus::registerProperty("Serial").withGetter([this]() { return this->nativePath; }),
              sdbus::registerProperty("Kind").withGetter([]() { return std::string{"battery"}; }),
              sdbus::registerProperty("PowerSupply").withGetter([this]() { return this->powerSupply; }),
              sdbus::registerProperty("EnergyFullWh").withGetter([]() { return 50.0; }),
              sdbus::registerProperty("EnergyFullDesignWh").withGetter([]() { return 55.0; }),
              sdbus::registerProperty("Percent").withGetter([this]() {
                std::scoped_lock lock(stateMutex);
                ++percentageReads;
                return percentage;
              }),
              sdbus::registerProperty("Present").withGetter([this]() { return present; }),
              sdbus::registerProperty("State").withGetter([]() { return std::string{"discharging"}; }),
              sdbus::registerProperty("TimeToEmptyS").withGetter([]() { return std::int64_t{7200}; }),
              sdbus::registerProperty("TimeToFullS").withGetter([]() { return std::int64_t{0}; }),
              sdbus::registerProperty("RateW").withGetter([]() { return 8.0; }),
              sdbus::registerProperty("EnergyWh").withGetter([]() { return 30.0; }),
              sdbus::registerProperty("ChargeLimit").withGetter([this]() {
                std::scoped_lock lock(stateMutex);
                ++chargeLimitReads;
                return ChargeLimit{thresholdSupported, thresholdEnabled, 75U, 80U, supportedSettings};
              }),
              sdbus::registerMethod("SetChargeLimit")
                  .withInputParamNames("enabled", "start", "end")
                  .implementedAs([this](sdbus::Result<>&& result, bool /*enabled*/, std::uint32_t, std::uint32_t) {
                    std::scoped_lock lock(stateMutex);
                    pendingResults.push_back(std::move(result));
                  })
          )
          .forInterface(kDeviceInterface);
    }

    void disableThresholdSupport() {
      std::scoped_lock lock(stateMutex);
      thresholdSupported = false;
      supportedSettings = 0;
    }

    void enableThresholdSupport() {
      std::scoped_lock lock(stateMutex);
      thresholdSupported = true;
      supportedSettings = 3;
    }

    void completeSuccess(bool enabled) {
      std::optional<sdbus::Result<>> result;
      {
        std::scoped_lock lock(stateMutex);
        TEST_CHECK(!pendingResults.empty());
        thresholdEnabled = enabled;
        result.emplace(std::move(pendingResults.front()));
        pendingResults.erase(pendingResults.begin());
      }
      result->returnResults();
    }

    void completeFailure(std::string message) {
      std::optional<sdbus::Result<>> result;
      {
        std::scoped_lock lock(stateMutex);
        TEST_CHECK(!pendingResults.empty());
        result.emplace(std::move(pendingResults.front()));
        pendingResults.erase(pendingResults.begin());
      }
      result->returnError(sdbus::Error{sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, std::move(message)});
    }

    void emitChanged() { object->emitPropertiesChangedSignal(kDeviceInterface); }
    void emitChanged(std::string property) {
      object->emitPropertiesChangedSignal(
          kDeviceInterface, std::vector<sdbus::PropertyName>{sdbus::PropertyName{std::move(property)}}
      );
    }
    void setPercentage(double value) {
      std::scoped_lock lock(stateMutex);
      percentage = value;
    }
    std::size_t pendingCount() const {
      std::scoped_lock lock(stateMutex);
      return pendingResults.size();
    }
    int chargeLimitReadCount() const {
      std::scoped_lock lock(stateMutex);
      return chargeLimitReads;
    }
    int percentageReadCount() const {
      std::scoped_lock lock(stateMutex);
      return percentageReads;
    }
    sdbus::IConnection& connection;
    std::string path;
    std::string nativePath;
    std::string model;
    bool powerSupply = true;
    bool present = true;
    mutable std::mutex stateMutex;
    bool thresholdSupported = true;
    bool thresholdEnabled = false;
    std::uint32_t supportedSettings = 3;
    double percentage = 55.0;
    int chargeLimitReads = 0;
    int percentageReads = 0;
    std::vector<sdbus::Result<>> pendingResults;
    std::unique_ptr<sdbus::IObject> object;
  };

  // sd-bus answers GetManagedObjects for the root and its children; batteries announce themselves with
  // InterfacesAdded / InterfacesRemoved.
  class FakeSystem {
  public:
    FakeSystem()
        : connection(sdbus::createSessionBusConnection(kBusName)), root(sdbus::createObject(*connection, kRootPath)),
          power(sdbus::createObject(*connection, kPowerPath)) {
      root->addObjectManager();
      power->addVTable(sdbus::registerProperty("OnBattery").withGetter([]() { return false; }))
          .forInterface(kPowerInterface);
      connection->enterEventLoopAsync();
    }

    ~FakeSystem() { connection->leaveEventLoop(); }

    FakeBattery& addBattery(std::string id, std::string model, bool emitSignal = false, bool powerSupply = true) {
      const std::string path = "/org/andromeda/System1/Battery/battery_" + id;
      batteries.push_back(
          std::make_unique<FakeBattery>(*connection, path, "NOCTALIA_TEST_" + id, std::move(model), powerSupply)
      );
      if (emitSignal) {
        batteries.back()->object->emitInterfacesAddedSignal();
      }
      return *batteries.back();
    }

    void removeBattery(FakeBattery& battery) const {
      battery.object->emitInterfacesRemovedSignal();
      battery.object->unregister();
    }

    void readdBattery(FakeBattery& battery) const {
      battery.object.reset();
      battery.registerObject();
      battery.object->emitInterfacesAddedSignal();
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> root;
    std::unique_ptr<sdbus::IObject> power;
    std::vector<std::unique_ptr<FakeBattery>> batteries;
  };

  template <typename Predicate> void drainUntil(SystemBus& bus, Predicate&& predicate) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
      bus.processPendingEvents();
      if (std::invoke(predicate)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noctalia::test::failCheck("timed out waiting for D-Bus state");
  }

  const UPowerDeviceInfo* findBattery(const UPowerService& service, const std::string& path) {
    for (const auto& battery : service.batteryDevices()) {
      if (battery.path == path) {
        return service.deviceForSelector(path);
      }
    }
    return nullptr;
  }

} // namespace

class PowerTabTestAccess {
public:
  static void rebuild(PowerTab& tab) { tab.rebuildChargeLimits(); }
  static std::size_t rowCount(const PowerTab& tab) { return tab.m_chargeLimitRows.size(); }
  static bool nameVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).nameLabel->visible();
  }
  static std::string name(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).nameLabel->text();
  }
  static bool toggleChecked(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).toggle->checked();
  }
  static bool toggleEnabled(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).toggle->enabled();
  }
  static bool errorVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).errorLabel->visible();
  }
  static std::string error(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).errorLabel->text();
  }
  static std::string behavior(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).behaviorLabel->text();
  }
  static bool configuredVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).configuredLabel->visible();
  }
  static bool managementVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).managementLabel->visible();
  }
  static bool controlVisible(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).controlRow->visible();
  }
  static float behaviorOpacity(const PowerTab& tab, std::size_t index) {
    return tab.m_chargeLimitRows.at(index).behaviorLabel->color().a;
  }
};

int main() {
  i18n::Service::instance().init("en");
  FakeSystem fake;
  auto& bat0 = fake.addBattery("BAT0", "Primary Battery");
  auto& bat1 = fake.addBattery("BAT1", "Secondary Battery");
  auto& mouse = fake.addBattery("MOUSE0", "Wireless Mouse", false, false);
  auto& hiddenBattery = fake.addBattery("UNSUPPORTED", "Unsupported Battery");
  hiddenBattery.disableThresholdSupport();

  SystemBus bus;
  {
    UPowerService service(bus);
    bool initialSeen = false;
    int deviceChanges = 0;
    int chargeLimitChanges = 0;
    service.setChangeCallback([&](const UPowerChange& change) {
      if (change.origin == UPowerService::ChangeOrigin::Initial) {
        initialSeen = true;
      } else if (change.origin == UPowerService::ChangeOrigin::DeviceState) {
        ++deviceChanges;
      } else {
        ++chargeLimitChanges;
      }
    });

    drainUntil(bus, [&]() { return initialSeen; });
    PowerTab tab(&service, nullptr);
    auto root = tab.create();
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::rowCount(tab) == 2);
    TEST_CHECK(PowerTabTestAccess::nameVisible(tab, 0));
    TEST_CHECK(PowerTabTestAccess::nameVisible(tab, 1));
    TEST_CHECK(PowerTabTestAccess::name(tab, 0) == "Primary Battery");
    TEST_CHECK(PowerTabTestAccess::name(tab, 1) == "Secondary Battery");
    TEST_CHECK(findBattery(service, hiddenBattery.path) != nullptr);
    TEST_CHECK(PowerTabTestAccess::behavior(tab, 0) == "Can charge to 100%");
    TEST_CHECK(PowerTabTestAccess::behaviorOpacity(tab, 0) == colorForRole(ColorRole::Label1).a);
    TEST_CHECK(!PowerTabTestAccess::configuredVisible(tab, 0));
    TEST_CHECK(!PowerTabTestAccess::managementVisible(tab, 0));
    TEST_CHECK(PowerTabTestAccess::controlVisible(tab, 0));
    // sd-bus's GetManagedObjects reads every property; after it, a change that would refresh a laptop battery's
    // charge limit (PowerSupply) must not make the shell read a peripheral's.
    const int mouseChargeLimitReads = mouse.chargeLimitReadCount();
    const int mousePercentReads = mouse.percentageReadCount();
    mouse.emitChanged("PowerSupply");
    drainUntil(bus, [&]() { return mouse.percentageReadCount() > mousePercentReads; });
    TEST_CHECK(mouse.chargeLimitReadCount() == mouseChargeLimitReads);

    const int bat0SupportedBeforePercentage = bat0.chargeLimitReadCount();
    const int bat0PercentageBeforePercentage = bat0.percentageReadCount();
    const int bat1SupportedBeforePercentage = bat1.chargeLimitReadCount();
    const int bat1PercentageBeforePercentage = bat1.percentageReadCount();
    bat0.setPercentage(57.0);
    bat0.emitChanged("Percent");
    drainUntil(bus, [&]() {
      const auto* current = service.deviceForSelector(bat0.path);
      return current != nullptr && current->state.percentage == 57.0;
    });
    TEST_CHECK(bat0.percentageReadCount() > bat0PercentageBeforePercentage);
    TEST_CHECK(bat0.chargeLimitReadCount() == bat0SupportedBeforePercentage);
    TEST_CHECK(bat1.percentageReadCount() == bat1PercentageBeforePercentage);
    TEST_CHECK(bat1.chargeLimitReadCount() == bat1SupportedBeforePercentage);

    const int bat0SupportedBeforeThreshold = bat0.chargeLimitReadCount();
    const int bat1SupportedBeforeThreshold = bat1.chargeLimitReadCount();
    const int bat1PercentageBeforeThreshold = bat1.percentageReadCount();
    bat0.emitChanged("ChargeLimit");
    drainUntil(bus, [&]() { return bat0.chargeLimitReadCount() > bat0SupportedBeforeThreshold; });
    TEST_CHECK(bat1.chargeLimitReadCount() == bat1SupportedBeforeThreshold);
    TEST_CHECK(bat1.percentageReadCount() == bat1PercentageBeforeThreshold);
    deviceChanges = 0;
    chargeLimitChanges = 0;

    const auto* initial = findBattery(service, bat0.path);
    TEST_CHECK(initial != nullptr);
    TEST_CHECK(initial->chargeLimit.supported);
    TEST_CHECK(initial->chargeLimit.methodAvailable);
    TEST_CHECK(initial->chargeLimit.enabledAvailable);
    TEST_CHECK(service.enableChargeThreshold(bat0.path, true));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    const auto* info = findBattery(service, bat0.path);
    TEST_CHECK(info != nullptr && info->chargeLimit.requestPending);
    TEST_CHECK(info->chargeLimit.requestedEnabled == true);
    TEST_CHECK(chargeLimitChanges >= 1 && deviceChanges == 0);
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::toggleChecked(tab, 0));
    TEST_CHECK(!PowerTabTestAccess::toggleEnabled(tab, 0));
    TEST_CHECK(PowerTabTestAccess::behavior(tab, 0) == "Starts below 75% · Stops at 80%");
    TEST_CHECK(!PowerTabTestAccess::configuredVisible(tab, 0));
    TEST_CHECK(!PowerTabTestAccess::managementVisible(tab, 0));
    TEST_CHECK(PowerTabTestAccess::controlVisible(tab, 0));

    const int changesBeforeSuccess = chargeLimitChanges;
    bat0.completeSuccess(true);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending && current->chargeLimit.enabled;
    });
    info = findBattery(service, bat0.path);
    TEST_CHECK(info != nullptr && !info->chargeLimit.requestedEnabled.has_value());
    TEST_CHECK(info->chargeLimit.operationError == ChargeLimitOperationError::None);
    TEST_CHECK(deviceChanges == 0);
    TEST_CHECK(chargeLimitChanges == changesBeforeSuccess + 1);
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::toggleChecked(tab, 0));
    TEST_CHECK(PowerTabTestAccess::toggleEnabled(tab, 0));
    TEST_CHECK(PowerTabTestAccess::behavior(tab, 0) == "Starts below 75% · Stops at 80%");
    TEST_CHECK(PowerTabTestAccess::behaviorOpacity(tab, 0) == colorForRole(ColorRole::Label1).a);

    TEST_CHECK(service.enableChargeThreshold(bat0.path, false));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::behavior(tab, 0) == "Can charge to 100%");
    TEST_CHECK(PowerTabTestAccess::behaviorOpacity(tab, 0) == colorForRole(ColorRole::Label1).a);
    TEST_CHECK(!PowerTabTestAccess::configuredVisible(tab, 0));
    const int enabledReadsBeforeFailure = bat0.chargeLimitReadCount();
    bat0.completeFailure("hardware rejected the write");
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending;
    });
    info = findBattery(service, bat0.path);
    TEST_CHECK(info != nullptr && info->chargeLimit.enabled);
    TEST_CHECK(info->chargeLimit.operationError == ChargeLimitOperationError::Failed);
    TEST_CHECK(bat0.chargeLimitReadCount() > enabledReadsBeforeFailure);
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::toggleChecked(tab, 0));
    TEST_CHECK(PowerTabTestAccess::toggleEnabled(tab, 0));
    TEST_CHECK(PowerTabTestAccess::errorVisible(tab, 0));
    TEST_CHECK(PowerTabTestAccess::error(tab, 0) == "Charge thresholds could not be changed.");

    TEST_CHECK(service.enableChargeThreshold(bat0.path, false));
    drainUntil(bus, [&]() { return bat0.pendingCount() == 1; });
    bat0.completeFailure("Operation is not allowed.");
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat0.path);
      return current != nullptr && !current->chargeLimit.requestPending;
    });
    info = findBattery(service, bat0.path);
    TEST_CHECK(info != nullptr && info->chargeLimit.operationError == ChargeLimitOperationError::PermissionDenied);
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::error(tab, 0) == "Authorization was denied. Charge thresholds were not changed.");

    TEST_CHECK(service.enableChargeThreshold(bat1.path, true));
    drainUntil(bus, [&]() { return bat1.pendingCount() == 1; });
    fake.removeBattery(bat1);
    drainUntil(bus, [&]() { return findBattery(service, bat1.path) == nullptr; });
    PowerTabTestAccess::rebuild(tab);
    TEST_CHECK(PowerTabTestAccess::rowCount(tab) == 1);
    TEST_CHECK(!PowerTabTestAccess::nameVisible(tab, 0));
    fake.readdBattery(bat1);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat1.path);
      return current != nullptr && current->chargeLimit.methodAvailable;
    });
    TEST_CHECK(service.enableChargeThreshold(bat1.path, true));
    drainUntil(bus, [&]() { return bat1.pendingCount() == 2; });
    const int changesBeforeStaleReply = chargeLimitChanges;
    bat1.completeFailure("stale request failed");
    for (int i = 0; i < 50; ++i) {
      bus.processPendingEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    info = findBattery(service, bat1.path);
    TEST_CHECK(info != nullptr && info->chargeLimit.requestPending);
    TEST_CHECK(info->chargeLimit.operationError == ChargeLimitOperationError::None);
    TEST_CHECK(chargeLimitChanges == changesBeforeStaleReply);
    bat1.completeSuccess(true);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat1.path);
      return current != nullptr && !current->chargeLimit.requestPending && current->chargeLimit.enabled;
    });
  }

  // A retained async proxy must not dispatch its signal or reply callback into a destroyed service.
  fake.removeBattery(bat0);
  auto& bat2 = fake.addBattery("BAT2", "Lifetime Battery");
  {
    UPowerService service(bus);
    drainUntil(bus, [&]() {
      const auto* current = findBattery(service, bat2.path);
      return current != nullptr && current->chargeLimit.methodAvailable;
    });
    TEST_CHECK(service.enableChargeThreshold(bat2.path, true));
    drainUntil(bus, [&]() { return bat2.pendingCount() == 1; });
  }
  bat2.emitChanged();
  bat2.completeSuccess(true);
  for (int i = 0; i < 20; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return 0;
}
