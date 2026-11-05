#include "dbus/system_interactive_call.h"

#include <gio/gio.h>

#include <memory>
#include <utility>

namespace dbus {

  void callInteractive(const std::string& service, const std::string& path, const char* iface, const char* method,
                       GVariant* args, InteractiveReply reply) {
    // A polkit prompt waits for the user; the D-Bus default of 25 s is too short.
    constexpr int kAuthTimeoutMs = 5 * 60 * 1000;
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (bus == nullptr) {
      std::string message = error->message;
      g_error_free(error);
      g_variant_unref(g_variant_ref_sink(args));
      reply(nullptr, std::move(message));
      return;
    }
    g_dbus_connection_call(
        bus, service.c_str(), path.c_str(), iface, method, args, nullptr,
        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, kAuthTimeoutMs, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<InteractiveReply> callback(static_cast<InteractiveReply*>(data));
          GError* callError = nullptr;
          GVariant* value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &callError);
          if (value == nullptr) {
            g_dbus_error_strip_remote_error(callError);
            std::string message = callError->message;
            g_error_free(callError);
            (*callback)(nullptr, std::move(message));
            return;
          }
          (*callback)(value, {});
          g_variant_unref(value);
        },
        new InteractiveReply(std::move(reply))
    );
    g_object_unref(bus);
  }

} // namespace dbus
