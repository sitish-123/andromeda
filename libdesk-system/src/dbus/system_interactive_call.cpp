#include "dbus/system_interactive_call.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <systemd/sd-bus.h>
#include <utility>

namespace dbus {

  namespace {

    constexpr Logger kLog("system_bus");

    void deleteReply(void* userdata) { delete static_cast<InteractiveReply*>(userdata); }

    int onReply(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
      auto& reply = *static_cast<InteractiveReply*>(userdata);
      // sd-bus calls this from C; nothing may unwind through it.
      try {
        if (const sd_bus_error* error = sd_bus_message_get_error(message); error != nullptr) {
          reply(nullptr, error->message != nullptr ? error->message : error->name);
        } else {
          reply(message, {});
        }
      } catch (const std::exception& e) {
        kLog.warn("interactive call reply threw: {}", e.what());
      }
      return 0;
    }

  } // namespace

  void callInteractive(SystemBus& bus, const char* service, const char* path, const char* iface, const char* method,
                       const InteractiveArgs& args, InteractiveReply reply) {
    // A polkit prompt waits for the user; the D-Bus default of 25 s is too short.
    constexpr std::uint64_t kAuthTimeoutUsec = 5ULL * 60 * 1000 * 1000;
    sd_bus_message* call = nullptr;
    int r = sd_bus_message_new_method_call(bus.rawBus(), &call, service, path, iface, method);
    if (r >= 0) {
      r = sd_bus_message_set_allow_interactive_authorization(call, 1);
    }
    if (r >= 0) {
      r = args(call);
    }
    sd_bus_slot* slot = nullptr;
    auto* owned = new InteractiveReply(std::move(reply));
    if (r >= 0) {
      r = sd_bus_call_async(bus.rawBus(), &slot, call, onReply, owned, kAuthTimeoutUsec);
    }
    sd_bus_message_unref(call);
    if (r < 0) {
      const std::string message = std::strerror(-r);
      (*owned)(nullptr, message);
      delete owned;
      return;
    }
    sd_bus_slot_set_destroy_callback(slot, deleteReply);
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
  }

} // namespace dbus
