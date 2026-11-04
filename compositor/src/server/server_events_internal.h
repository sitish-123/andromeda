#pragma once

#include "wlr.h"

namespace andromeda {

  class Server;
  class View;

  namespace server_events {
    View* viewForToplevel(Server& server, wlr_xdg_toplevel* toplevel);
    void applyPointerConfig(wlr_input_device* device);
  } // namespace server_events

} // namespace andromeda
