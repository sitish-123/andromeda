#pragma once

#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"

#include <memory>

struct dsk_surface_manager_v1;
struct wl_surface;

// One glass system (CLAUDE.md Decisions): sends a surface's glass to the compositor, the glass-slider level for a
// small liquid shape or the rim alone for a sheet, and sends it again when the slider or the theme changes. For
// surfaces with no DskSurfaceClient of their own; a surface has at most one.
class GlassSurface {
public:
  enum class Kind { Shape, Sheet };

  // Binds to `surface` once; later calls with the same surface only resend. Requests apply on the next commit.
  void apply(dsk_surface_manager_v1* manager, wl_surface* surface, Kind kind) {
    if (manager == nullptr || surface == nullptr) {
      return;
    }
    if (m_surface != surface) {
      m_dsk = std::make_unique<DskSurfaceClient>(manager, surface);
      m_surface = surface;
      m_paletteConn = paletteChanged().connect([this] { send(); });
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
    const bool glass = !Style::reduceTransparency();
    const double level = !glass ? 0.0 : m_kind == Kind::Shape ? Style::liquidLevel() : Style::sheetLevel();
    m_dsk->set(DskSurfaceClient::Property::Liquid, {level, glass ? 1.0 : 0.0, 0.0, 0.0});
  }

  std::unique_ptr<DskSurfaceClient> m_dsk;
  wl_surface* m_surface = nullptr;
  Kind m_kind = Kind::Sheet;
  Signal<>::ScopedConnection m_paletteConn;
};
