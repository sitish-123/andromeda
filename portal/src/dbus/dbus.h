#pragma once

#include <memory>

namespace xdpu {

  class Loop;
  class PipeWireContext;
  class WaylandContext;
  struct Config;

  class DbusPortal {
  public:
    DbusPortal(Loop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire);
    ~DbusPortal();

    DbusPortal(const DbusPortal&) = delete;
    DbusPortal& operator=(const DbusPortal&) = delete;
    DbusPortal(DbusPortal&&) = delete;
    DbusPortal& operator=(DbusPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
