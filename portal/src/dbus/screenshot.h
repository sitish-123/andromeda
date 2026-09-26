#pragma once

#include <memory>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

namespace xdpu {

  class Loop;
  class WaylandContext;
  struct Config;

  class ScreenshotPortal {
  public:
    ScreenshotPortal(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config,
        WaylandContext& wayland
    );
    ~ScreenshotPortal();

    ScreenshotPortal(const ScreenshotPortal&) = delete;
    ScreenshotPortal& operator=(const ScreenshotPortal&) = delete;
    ScreenshotPortal(ScreenshotPortal&&) = delete;
    ScreenshotPortal& operator=(ScreenshotPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
