#pragma once

#include "ui/auth/auth_column.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/wayland_connection.h"

#include <memory>
#include <utility>

// Sends the clock's glass mask (AuthColumn::clockGlassMask) on its surface's dsk_surface_v1 when it changes; the
// mask applies with the surface's next commit. The surface must have no other DskSurfaceClient. A new surface (an
// editor opened again) gets a new dsk_surface_v1.
class ClockGlassSync {
public:
  void sync(WaylandConnection& connection, wl_surface* surface, AuthColumn::GlassMask mask) {
    if (surface == nullptr) {
      return;
    }
    if (surface != m_surface) {
      m_dsk.reset();
      m_sent = {};
      m_surface = surface;
    }
    if (mask == m_sent) {
      return;
    }
    if (m_dsk == nullptr) {
      if (mask.alpha.empty() || connection.dskSurfaceManager() == nullptr) {
        m_sent = std::move(mask);
        return;
      }
      m_dsk = std::make_unique<DskSurfaceClient>(connection.dskSurfaceManager(), surface);
    }
    (void)m_dsk->setGlassMask(
        connection.shm(), mask.alpha, mask.pixelWidth, mask.pixelHeight, mask.x, mask.y, mask.width, mask.height
    );
    m_sent = std::move(mask);
  }

private:
  wl_surface* m_surface = nullptr;
  std::unique_ptr<DskSurfaceClient> m_dsk;
  AuthColumn::GlassMask m_sent;
};
