#include "core/scanout_keeper.h"

#include "core/log.h"
#include "wlr.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

namespace andromeda::scanout_keeper {

  namespace {
    constexpr Logger kLog("scanout");
    constexpr const char* kService = "org.andromeda.System1";
    constexpr const char* kPath = "/org/andromeda/System1/Display";
    constexpr const char* kInterface = "org.andromeda.System1.Display";
    constexpr uint64_t kHoldTimeoutUs = 500'000;

    int g_pendingOutputs = 0;
    bool g_released = false;

    void holdBackend(wlr_backend* backend, void* data) {
      if (!wlr_backend_is_drm(backend)) {
        return;
      }
      auto* bus = static_cast<sd_bus*>(data);
      sd_bus_error error = SD_BUS_ERROR_NULL;
      const int result = sd_bus_call_method(
          bus, kService, kPath, kInterface, "HoldScanout", &error, nullptr, "h", wlr_backend_get_drm_fd(backend)
      );
      if (result < 0) {
        kLog.warn("scanout hand-off failed: {}", error.message != nullptr ? error.message : std::strerror(-result));
      }
      sd_bus_error_free(&error);
    }

    // greetd's hand-off waits for this connection before it ends the greeter session (session/greetd patches).
    void reportHandoffReady() {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      if (runtime == nullptr) {
        return;
      }
      const std::string path = std::string(runtime) + "/greetd-handoff.sock";
      sockaddr_un address{};
      if (path.size() >= sizeof(address.sun_path)) {
        return;
      }
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return;
      }
      if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        kLog.info("reported the first frame to greetd's hand-off");
      }
      close(fd);
    }

    void release() {
      g_released = true;
      reportHandoffReady();
      sd_bus* bus = nullptr;
      if (sd_bus_open_system(&bus) < 0) {
        return;
      }
      // Waits for the reply: the daemon checks polkit against this connection, which a fire-and-forget send closes
      // first, so the release was refused and the 15 s fallback dropped the greeter's planes on screen.
      sd_bus_set_method_call_timeout(bus, kHoldTimeoutUs);
      sd_bus_message* message = nullptr;
      sd_bus_message* reply = nullptr;
      sd_bus_error error = SD_BUS_ERROR_NULL;
      int result = sd_bus_message_new_method_call(bus, &message, kService, kPath, kInterface, "ReleaseScanout");
      if (result >= 0) {
        sd_bus_message_set_auto_start(message, 0);
        result = sd_bus_call(bus, message, 0, &error, &reply);
      }
      // No daemon on the bus: nothing is held.
      if (result < 0 && !sd_bus_error_has_names(&error, SD_BUS_ERROR_SERVICE_UNKNOWN, SD_BUS_ERROR_NAME_HAS_NO_OWNER)) {
        kLog.warn("scanout release failed: {}", error.message != nullptr ? error.message : std::strerror(-result));
      }
      sd_bus_error_free(&error);
      sd_bus_message_unref(reply);
      sd_bus_message_unref(message);
      sd_bus_flush_close_unref(bus);
    }
  } // namespace

  void holdForNextCompositor(wlr_backend* backend) {
    // Outputs torn down after this must not release the hold just placed.
    g_released = true;
    if (backend == nullptr) {
      return;
    }
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0) {
      return;
    }
    sd_bus_set_method_call_timeout(bus, kHoldTimeoutUs);
    if (wlr_backend_is_multi(backend)) {
      wlr_multi_for_each_backend(backend, holdBackend, bus);
    } else {
      holdBackend(backend, bus);
    }
    sd_bus_flush_close_unref(bus);
  }

  void expectFirstFrame() {
    if (!g_released) {
      ++g_pendingOutputs;
    }
  }

  void firstFrameShown() {
    if (!g_released && g_pendingOutputs > 0 && --g_pendingOutputs == 0) {
      release();
    }
  }

} // namespace andromeda::scanout_keeper
