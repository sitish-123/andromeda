#pragma once

namespace andromeda {

  // `andromeda update`: checks every source through org.andromeda.Update1 (session bus), lists the updates, then
  // installs them through org.andromeda.UpdateHelper1 (system bus, polkit), as the Software Update page does.
  int runUpdate();

} // namespace andromeda
