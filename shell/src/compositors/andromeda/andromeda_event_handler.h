#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace compositors::andromeda {

  class AndromedaRuntime;

  // Base for objects that consume Andromeda event-stream messages. Handlers
  // register themselves with the runtime on construction and unregister on
  // destruction. The socket owner (AndromedaWorkspaceBackend) reads the stream
  // and fans each parsed event out through the runtime to every registered
  // handler.
  class AndromedaEventHandler {
  public:
    explicit AndromedaEventHandler(AndromedaRuntime& runtime);
    virtual ~AndromedaEventHandler();

    AndromedaEventHandler(const AndromedaEventHandler&) = delete;
    AndromedaEventHandler& operator=(const AndromedaEventHandler&) = delete;

    virtual void handleEvent(std::string_view event, const nlohmann::json& data) = 0;
    // Called when the runtime tears the event stream down (e.g. cleanup): the
    // handler's cached state should be considered stale.
    virtual void handleStreamReset() {}

  protected:
    AndromedaRuntime& m_runtime;
  };

} // namespace compositors::andromeda
