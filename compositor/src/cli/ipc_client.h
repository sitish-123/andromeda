#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  struct IpcCommandSpec;

  int runIpcCommand(const IpcCommandSpec& spec, std::string_view arg = {}, bool json = false);

  // Holds the connection open and relays every event line to stdout until the compositor closes it. Returns failure
  // when a name in `events` is not a subscribable family.
  int runIpcSubscribe(const std::vector<std::string>& events);

} // namespace umbriel
