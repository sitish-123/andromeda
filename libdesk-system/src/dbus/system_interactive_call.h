#pragma once

#include <functional>
#include <string>

typedef struct _GVariant GVariant;

namespace dbus {

  // `result` is null on failure, with the error text in `error`; it is borrowed for the call only.
  using InteractiveReply = std::function<void(GVariant* result, std::string error)>;

  // A system-bus method call that lets the service ask polkit for a password. GDBus, not sdbus-c++: only GDBus sets
  // ALLOW_INTERACTIVE_AUTHORIZATION, without which a service denies at once. The reply runs on the default GLib
  // context, which the polkit agent pumps; `args` (floating) is consumed.
  void callInteractive(const std::string& service, const std::string& path, const char* iface, const char* method,
                       GVariant* args, InteractiveReply reply);

} // namespace dbus
