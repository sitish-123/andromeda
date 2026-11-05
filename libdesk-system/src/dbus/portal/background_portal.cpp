#include "dbus/portal/background_portal.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <utility>

namespace {

  constexpr Logger kLog("background-portal");

  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Background";
  constexpr std::uint32_t kResponseSuccess = 0;
  constexpr std::uint32_t kResultAllow = 1;

} // namespace

BackgroundPortal::BackgroundPortal(
    SessionBus& bus, std::function<AppStates()> appStates,
    std::function<void(const std::string& appId, const std::string& name)> notifyBackground
)
    : m_bus(bus), m_appStates(std::move(appStates)), m_notifyBackground(std::move(notifyBackground)) {
  m_lastStates = m_appStates();
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("GetAppState").withOutputParamNames("apps").implementedAs([this]() {
            std::map<std::string, sdbus::Variant> apps;
            for (const auto& [appId, state] : m_appStates()) {
              apps.emplace(appId, sdbus::Variant{state});
            }
            return apps;
          }),
          sdbus::registerMethod("NotifyBackground")
              .withInputParamNames("handle", "app_id", "name")
              .withOutputParamNames("response", "results")
              .implementedAs([this](const sdbus::ObjectPath& /*handle*/, const std::string& appId, const std::string& name) {
                m_notifyBackground(appId, name);
                const std::map<std::string, sdbus::Variant> results{{"result", sdbus::Variant{kResultAllow}}};
                return std::tuple{kResponseSuccess, results};
              }),
          sdbus::registerMethod("EnableAutostart")
              .withInputParamNames("app_id", "enable", "commandline", "flags")
              .withOutputParamNames("result")
              .implementedAs([](const std::string& appId, bool /*enable*/, const std::vector<std::string>& /*commandline*/,
                                std::uint32_t /*flags*/) {
                kLog.info("EnableAutostart for {} ignored: xdg-desktop-portal writes autostart files", appId);
                return false;
              }),
          sdbus::registerSignal("RunningApplicationsChanged")
      )
      .forInterface(kInterface);
  kLog.info("serving {}", kInterface);
}

BackgroundPortal::~BackgroundPortal() = default;

void BackgroundPortal::onToplevelsChanged() {
  AppStates states = m_appStates();
  if (states == m_lastStates) {
    return;
  }
  m_lastStates = std::move(states);
  m_object->emitSignal("RunningApplicationsChanged").onInterface(kInterface);
}
