#include "app/app_loop.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "foreign_parent.h"
#include "preview_service.h"
#include "preview_window.h"

#include <chrono>
#include <cstring>
#include <gio/gio.h>

namespace {
  constexpr Logger kLog("preview");
  // A run of previews pays the start once (docs/search.md §5).
  constexpr std::chrono::minutes kIdleExit{5};

  std::string localPath(const std::string& uri) {
    GFile* file = g_file_new_for_uri(uri.c_str());
    char* path = g_file_is_native(file) ? g_file_get_path(file) : nullptr;
    std::string out = path != nullptr ? path : "";
    g_free(path);
    g_object_unref(file);
    return out;
  }
} // namespace

int main(int argc, char* argv[]) {
  AppLoop loop;
  if (!loop.initialize()) {
    kLog.error("no Wayland compositor");
    return 1;
  }
  SessionBus bus;
  SessionBusPollSource busSource(bus);
  loop.addSource(&busSource);

  Timer idleExit;
  auto armIdleExit = [&] { idleExit.start(kIdleExit, [&] { loop.quit(0); }); };

  preview::ForeignParent parent(loop.wayland().display());
  preview::PreviewService* servicePtr = nullptr;
  preview::PreviewWindow window(
      loop, parent,
      {.closed = [&] { armIdleExit(); },
       .selection = [&](std::int32_t direction) {
         if (servicePtr != nullptr) {
           servicePtr->emitSelectionEvent(direction);
         }
       }}
  );
  preview::PreviewService service(
      bus.connection(),
      {.showFile =
           [&](const std::string& uri, const std::string& handle, bool closeIfShown) {
             // Only local files: the helper and glycin open paths, never remote URIs.
             const std::string path = uri.starts_with("file:") ? localPath(uri) : std::string{};
             if (path.empty()) {
               kLog.info("ignoring non-local uri {}", uri);
               return;
             }
             idleExit.stop();
             window.show(path, handle, closeIfShown);
           },
       .close = [&] { window.close(); }}
  );
  servicePtr = &service;
  if (!service.start()) {
    return 1;
  }

  // `andromeda-preview <file>` opens one file without a bus caller (development and tests).
  if (argc >= 2 && std::strlen(argv[1]) > 0) {
    GFile* file = g_file_new_for_commandline_arg(argv[1]);
    char* path = g_file_get_path(file);
    if (path != nullptr) {
      window.show(path, {}, false);
    }
    g_free(path);
    g_object_unref(file);
  } else {
    armIdleExit();
  }
  const int code = loop.run();
  loop.removeSource(&busSource);
  return code;
}
