#pragma once

#include <string>

class IpcClient {
public:
  // Sends `command` to the running noctalia instance.
  // Prints the response to stdout. Returns 0 on success, 1 on error (an "error:" reply or a JSON reply with ok false).
  // `stream`: no read timeout; prints each chunk as it arrives until the instance closes (msg subscribe).
  static int send(const std::string& command, bool stream = false);
};
