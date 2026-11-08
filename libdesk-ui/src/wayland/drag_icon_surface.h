#pragma once

#include "wayland/surface.h"

#include <cstdint>

// The image under the pointer during a drag (wl_data_device start_drag icon): a role-less surface the compositor
// moves with the pointer.
class DragIconSurface final : public Surface {
public:
  DragIconSurface(WaylandConnection& connection, std::uint32_t width, std::uint32_t height, std::int32_t bufferScale);
  ~DragIconSurface() override;

  bool initialize() override;

private:
  std::uint32_t m_width;
  std::uint32_t m_height;
  std::int32_t m_scale;
};
