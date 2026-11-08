#include "wayland/drag_icon_surface.h"

#include "wayland/wayland_connection.h"

DragIconSurface::DragIconSurface(WaylandConnection& connection, std::uint32_t width, std::uint32_t height,
                                 std::int32_t bufferScale)
    : Surface(connection), m_width(width), m_height(height), m_scale(bufferScale) {}

DragIconSurface::~DragIconSurface() { m_connection.unregisterSurface(m_surface); }

bool DragIconSurface::initialize() {
  if (m_connection.compositor() == nullptr || m_width == 0 || m_height == 0 || !createWlSurface()) {
    return false;
  }
  setBufferScale(m_scale);
  setRunning(true);
  onConfigure(m_width, m_height);
  return true;
}
