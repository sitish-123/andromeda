#pragma once

#include <memory>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

namespace xdpu {

  class Loop;
  class PipeWireContext;
  class WaylandContext;
  struct Config;

  class ScreenCastPortal {
  public:
    ScreenCastPortal(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config,
        WaylandContext& wayland, PipeWireContext& pipewire
    );
    ~ScreenCastPortal();

    ScreenCastPortal(const ScreenCastPortal&) = delete;
    ScreenCastPortal& operator=(const ScreenCastPortal&) = delete;
    ScreenCastPortal(ScreenCastPortal&&) = delete;
    ScreenCastPortal& operator=(ScreenCastPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
