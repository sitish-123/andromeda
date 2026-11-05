#include "preview_service.h"

#include "core/log.h"

namespace preview {

  namespace {
    constexpr Logger kLog("preview-dbus");
    const sdbus::ServiceName kBusName{"org.gnome.NautilusPreviewer"};
    const sdbus::ObjectPath kObjectPath{"/org/gnome/NautilusPreviewer"};
    const sdbus::InterfaceName kInterface{"org.gnome.NautilusPreviewer2"};
  } // namespace

  PreviewService::PreviewService(sdbus::IConnection& connection, Handlers handlers)
      : m_connection(connection), m_handlers(std::move(handlers)) {}

  PreviewService::~PreviewService() {
    m_object.reset();
    try {
      m_connection.releaseName(kBusName);
    } catch (const sdbus::Error&) {
    }
  }

  bool PreviewService::start() {
    try {
      m_object = sdbus::createObject(m_connection, kObjectPath);
      m_object
          ->addVTable(
              sdbus::registerMethod("ShowFile")
                  .withInputParamNames("uri", "window_handle", "close_if_already_visible")
                  .implementedAs([this](const std::string& uri, const std::string& handle, bool closeIfShown) {
                    m_handlers.showFile(uri, handle, closeIfShown);
                  }),
              sdbus::registerMethod("Close").implementedAs([this] { m_handlers.close(); }),
              sdbus::registerSignal("SelectionEvent").withParameters<std::int32_t>("direction")
          )
          .forInterface(kInterface);
      m_connection.requestName(kBusName);
    } catch (const sdbus::Error& e) {
      kLog.warn("cannot own {}: {}", std::string{kBusName}, e.getMessage());
      m_object.reset();
      return false;
    }
    return true;
  }

  void PreviewService::emitSelectionEvent(std::int32_t direction) {
    if (m_object) {
      m_object->emitSignal("SelectionEvent").onInterface(kInterface).withArguments(direction);
    }
  }

} // namespace preview
