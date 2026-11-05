#include "core/scanout_keeper.h"

#include "core/log.h"
#include "wlr.h"

#include <cstring>

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

    void release() {
      g_released = true;
      sd_bus* bus = nullptr;
      if (sd_bus_open_system(&bus) < 0) {
        return;
      }
      sd_bus_message* message = nullptr;
      if (sd_bus_message_new_method_call(bus, &message, kService, kPath, kInterface, "ReleaseScanout") >= 0) {
        sd_bus_message_set_expect_reply(message, 0);
        sd_bus_message_set_auto_start(message, 0);
        sd_bus_send(bus, message, nullptr);
        sd_bus_message_unref(message);
      }
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
