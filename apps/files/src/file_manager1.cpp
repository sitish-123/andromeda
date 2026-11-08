#include "file_manager1.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>

namespace {

  constexpr Logger kLog("files");
  const sdbus::ServiceName kBusName{"org.freedesktop.FileManager1"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/FileManager1"};
  constexpr auto kInterface = "org.freedesktop.FileManager1";

} // namespace

FileManager1::FileManager1(SessionBus& bus, Handler handler) : m_bus(bus), m_handler(std::move(handler)) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  const auto method = [this](const char* name, Show show) {
    return sdbus::registerMethod(name)
        .withInputParamNames("URIs", "StartupId")
        .implementedAs([this, show](const std::vector<std::string>& uris, const std::string& startupId) {
          m_handler(show, uris, startupId);
        });
  };
  m_object
      ->addVTable(method("ShowFolders", Show::Folders), method("ShowItems", Show::Items),
                  method("ShowItemProperties", Show::Properties))
      .forInterface(kInterface);
  try {
    m_bus.connection().requestName(kBusName);
    m_owned = true;
  } catch (const sdbus::Error& e) {
    kLog.info("FileManager1 name not taken: {}", e.getMessage());
  }
}

FileManager1::~FileManager1() {
  if (m_owned) {
    try {
      m_bus.connection().releaseName(kBusName);
    } catch (const sdbus::Error&) {
    }
  }
}
