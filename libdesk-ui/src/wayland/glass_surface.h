#pragma once

#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"

#include <memory>
#include <optional>

struct dsk_surface_manager_v1;
struct wl_surface;

// The dsk_surface_v1 liquid value of a material: {level, rim, lens}.
[[nodiscard]] inline DskSurfaceClient::Value glassLiquid(const glass::Material& material) noexcept {
  return {material.level, material.rim, material.lens, 0.0};
}

// The dsk_surface_v1 tint value (v9): the tint the compositor draws over the blur, `role` at `alpha`; 0 with reduce
// transparency. A surface that sends alpha > 0 paints no material fill of its own.
[[nodiscard]] inline DskSurfaceClient::Value glassTintValue(ColorRole role, float alpha) noexcept {
  const Color& c = colorForRole(role);
  return {c.r, c.g, c.b, Style::reduceTransparency() ? 0.0 : alpha};
}

// Sends a surface's glass::Material to the compositor and sends it again when the slider or the theme changes. For
// surfaces with no DskSurfaceClient of their own; a surface has at most one.
class GlassSurface {
public:
  using Kind = glass::Kind;

  // Binds to `surface` once; later calls with the same surface only resend. Requests apply on the next commit.
  void apply(dsk_surface_manager_v1* manager, wl_surface* surface, Kind kind) {
    if (manager == nullptr || surface == nullptr) {
      return;
    }
    if (m_surface != surface) {
      m_dsk = std::make_unique<DskSurfaceClient>(manager, surface);
      m_surface = surface;
      m_paletteConn = paletteChanged().connect([this] { send(); });
      m_sent.reset();
    }
    m_kind = kind;
    send();
  }

  void reset() {
    m_paletteConn = {};
    m_dsk.reset();
    m_surface = nullptr;
  }

private:
  void send() {
    if (m_dsk == nullptr || !m_dsk->hasLiquidProperty()) {
      return;
    }
    // A light/dark fade emits paletteChanged every frame; the values only change with the glass settings.
    const DskSurfaceClient::Value next = glassLiquid(Style::glassMaterial(m_kind));
    if (next == m_sent) {
      return;
    }
    m_sent = next;
    m_dsk->set(DskSurfaceClient::Property::Liquid, next);
  }

  std::unique_ptr<DskSurfaceClient> m_dsk;
  wl_surface* m_surface = nullptr;
  Kind m_kind = Kind::Sheet;
  std::optional<DskSurfaceClient::Value> m_sent;
  Signal<>::ScopedConnection m_paletteConn;
};
