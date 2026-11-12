// The auto-hide slide moves the dock panel, with its shadow, off its surface toward the anchored edge, while the
// hidden hover strip stays on that screen edge (docs/architecture-v2.md §7).
#include "config/config_types.h"
#include "shell/dock/dock_geometry.h"
#include "tests/test_check.h"
#include "ui/shadow_style.h"
#include "wayland/surface.h"

#include <cmath>
#include <cstdint>

namespace {

  using shell::dock::DockPanelGeometry;
  using shell::dock::DockSurfaceGeometry;

  struct Rect {
    float x0;
    float y0;
    float x1;
    float y1;
  };

  ShadowDirection shadowToward(DockEdge edge) {
    switch (edge) {
    case DockEdge::Bottom:
      return ShadowDirection::Up;
    case DockEdge::Top:
      return ShadowDirection::Down;
    case DockEdge::Left:
      return ShadowDirection::Right;
    case DockEdge::Right:
      return ShadowDirection::Left;
    }
    return ShadowDirection::Center;
  }

  std::int32_t edgeMargin(const DockSurfaceGeometry& geometry, DockEdge edge) {
    switch (edge) {
    case DockEdge::Bottom:
      return geometry.marginBottom;
    case DockEdge::Top:
      return geometry.marginTop;
    case DockEdge::Left:
      return geometry.marginLeft;
    case DockEdge::Right:
      return geometry.marginRight;
    }
    return 0;
  }

  // Distance from the rect to the surface side on `edge`; negative once the rect is past that side.
  float gapToEdge(const Rect& r, DockEdge edge, float w, float h) {
    switch (edge) {
    case DockEdge::Bottom:
      return h - r.y1;
    case DockEdge::Top:
      return r.y0;
    case DockEdge::Left:
      return r.x0;
    case DockEdge::Right:
      return w - r.x1;
    }
    return 0.0F;
  }

  bool outsideSurface(const Rect& r, float w, float h) { return r.x1 <= 0.0F || r.y1 <= 0.0F || r.x0 >= w || r.y0 >= h; }

  Rect shifted(const Rect& r, float dx, float dy) { return {r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}; }

  void checkDock(DockEdge edge, std::int32_t iconSize, std::int32_t marginEdge, bool shadowOn) {
    DockConfig cfg;
    cfg.position = edge;
    cfg.iconSize = iconSize;
    cfg.marginEdge = marginEdge;
    cfg.shadow = shadowOn;
    cfg.autoHide = true;
    ShellConfig::ShadowConfig shadow;
    shadow.direction = shadowToward(edge);

    const DockSurfaceGeometry surface = shell::dock::computeSurfaceGeometry(cfg, shadow, 5, false, 1920, 1080);
    const auto w = static_cast<float>(surface.surfaceW);
    const auto h = static_cast<float>(surface.surfaceH);
    const DockPanelGeometry panel = shell::dock::computePanelGeometry(cfg, shadow, w, h);
    const Rect shown{panel.panelX, panel.panelY, panel.panelX + panel.panelW, panel.panelY + panel.panelH};

    // An auto-hide dock's surface sits on the screen edge, so its hover strip is reachable; a margin floats the panel.
    TEST_CHECK(edgeMargin(surface, edge) == 0);
    TEST_CHECK(std::abs(gapToEdge(shown, edge, w, h) - static_cast<float>(marginEdge)) < 0.5F);

    const auto [dx, dy] = shell::dock::computeHiddenSlideDelta(cfg, shadow, w, h, panel);
    const bool vertical = shell::dock::isVerticalEdge(edge);
    TEST_CHECK(vertical ? dy == 0.0F : dx == 0.0F);
    const Rect hidden = shifted(shown, dx, dy);
    TEST_CHECK(outsideSurface(hidden, w, h));
    TEST_CHECK(gapToEdge(hidden, edge, w, h) < 0.0F);
    if (shadowOn) {
      const auto offset = shadowDirectionOffset(shadow.direction);
      const Rect shadowRect = shifted(hidden, static_cast<float>(offset.x), static_cast<float>(offset.y));
      TEST_CHECK(outsideSurface(shadowRect, w, h));
    }

    const auto strip = shell::dock::computeInputRegion(
        cfg, panel, static_cast<int>(surface.surfaceW), static_cast<int>(surface.surfaceH), true, false
    );
    TEST_CHECK(strip.size() == 1);
    const InputRect s = strip.front();
    const Rect stripRect{
        static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.x + s.width),
        static_cast<float>(s.y + s.height)
    };
    TEST_CHECK(s.width > 0 && s.height > 0);
    TEST_CHECK(stripRect.x0 >= 0.0F && stripRect.y0 >= 0.0F && stripRect.x1 <= w && stripRect.y1 <= h);
    TEST_CHECK(gapToEdge(stripRect, edge, w, h) == 0.0F);
    TEST_CHECK(vertical ? s.height == static_cast<int>(surface.surfaceH) : s.width == static_cast<int>(surface.surfaceW));
    TEST_CHECK(vertical ? s.width < static_cast<int>(panel.panelW) : s.height < static_cast<int>(panel.panelH));

    const auto shownRegion = shell::dock::computeInputRegion(
        cfg, panel, static_cast<int>(surface.surfaceW), static_cast<int>(surface.surfaceH), false, false
    );
    TEST_CHECK(shownRegion.size() == 1);
    TEST_CHECK(
        shownRegion.front().x == static_cast<int>(std::lround(panel.panelX))
        && shownRegion.front().y == static_cast<int>(std::lround(panel.panelY))
        && shownRegion.front().width == static_cast<int>(std::lround(panel.panelW))
        && shownRegion.front().height == static_cast<int>(std::lround(panel.panelH))
    );
  }

} // namespace

int main() {
  constexpr DockEdge kEdges[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};
  for (const DockEdge edge : kEdges) {
    for (const std::int32_t iconSize : {24, 48, 96}) {
      for (const std::int32_t marginEdge : {0, 6, 20}) {
        checkDock(edge, iconSize, marginEdge, false);
        checkDock(edge, iconSize, marginEdge, true);
      }
    }
  }
  return 0;
}
