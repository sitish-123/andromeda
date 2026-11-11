#pragma once

#include <functional>
#include <string>

class SystemBus;
typedef struct sd_bus_message sd_bus_message;

namespace dbus {

  // `result` is null on failure, with the error text in `error`; it is borrowed for the call only.
  using InteractiveReply = std::function<void(sd_bus_message* result, std::string error)>;
  // Appends the call's arguments with sd_bus_message_append(3); returns its result.
  using InteractiveArgs = std::function<int(sd_bus_message* call)>;

  // A system-bus method call that lets the service ask polkit for a password. Raw sd-bus, not sdbus-c++: only the
  // raw message sets ALLOW_INTERACTIVE_AUTHORIZATION, without which a service denies at once. The reply runs when the
  // system bus poll source dispatches.
  void callInteractive(SystemBus& bus, const char* service, const char* path, const char* iface, const char* method,
                       const InteractiveArgs& args, InteractiveReply reply);

} // namespace dbus
