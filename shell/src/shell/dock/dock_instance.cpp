#include "shell/dock/dock_instance.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_items.h"
#include "ui/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/glass_surface.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"

#include <algorithm>
#include <cmath>
#include <wayland-client-protocol.h>

namespace shell::dock {

  namespace {

    [[nodiscard]] bool dockUsesSlideAutoHide(const DockConfig& cfg, const DockInstance& instance) noexcept {
      if (cfg.smartAutoHide) {
        return !instance.smartAutoHidePinnedVisible;
      }
      return cfg.autoHide;
    }

    [[nodiscard]] bool dockUsesAnyAutoHide(const DockConfig& cfg) noexcept { return cfg.autoHide || cfg.smartAutoHide; }

    [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
      if (assignmentKey.empty()) {
        return false;
      }
      if (!workspace.id.empty() && assignmentKey == workspace.id) {
        return true;
      }
      if (!workspace.name.empty() && assignmentKey == workspace.name) {
        return true;
      }
      if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
        return true;
      }
      return false;
    }

    [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
      const auto workspaces = platform.workspaces(output);
      const Workspace* active = nullptr;
      for (const auto& workspace : workspaces) {
        if (workspace.active) {
          active = &workspace;
          break;
        }
      }
      if (active == nullptr) {
        return false;
      }

      const auto assignments = platform.workspaceWindowAssignments(output);
      for (const auto& assignment : assignments) {
        if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
          return true;
        }
      }
      if (!assignments.empty()) {
        return false;
      }
      return active->occupied;
    }

    [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
      if (platform.hasOverviewState() && platform.isOverviewOpen()) {
        return true;
      }
      return !activeWorkspaceHasWindows(platform, output);
    }

    void applyDockScrollOffset(DockInstance& instance, const DockConfig& cfg) {
      if (instance.row == nullptr) {
        return;
      }
      instance.scrollOffset = std::clamp(instance.scrollOffset, 0.0F, instance.maxScrollOffset);
      if (shell::dock::isVerticalEdge(cfg.position)) {
        instance.row->setPosition(instance.row->x(), -instance.scrollOffset);
      } else {
        instance.row->setPosition(-instance.scrollOffset, instance.row->y());
      }
    }

  } // namespace

  void prepareFrame(
      DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks, bool needsUpdate,
      bool needsLayout
  ) {
    if (instance.surface == nullptr) {
      return;
    }

    const auto width = instance.surface->width();
    const auto height = instance.surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    deps.renderContext.makeCurrent(instance.surface->renderTarget());

    bool needsModelRebuild = false;
    if (needsUpdate || instance.sceneRoot == nullptr) {
      UiPhaseScope updatePhase(UiPhase::Update);
      if (callbacks.syncModel) {
        needsModelRebuild = callbacks.syncModel(instance);
      }
    }

    const bool needsSceneBuild = instance.sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height;
    if (needsSceneBuild || needsLayout || needsModelRebuild) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      if (needsModelRebuild && instance.sceneRoot != nullptr && callbacks.rebuildItems) {
        callbacks.rebuildItems(instance);
      }
      buildScene(instance, deps, callbacks);
    } else if (needsUpdate && callbacks.updateVisuals) {
      callbacks.updateVisuals(instance);
    }
  }

  void syncDockSlideLayerTransform(DockInstance& instance, const DockConfig& cfg) {
    if (instance.slideRoot == nullptr) {
      return;
    }
    if (dockUsesSlideAutoHide(cfg, instance) || instance.hideOpacity < 0.999F) {
      const float t = 1.0F - instance.hideOpacity;
      instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
    } else {
      instance.slideRoot->setPosition(0.0F, 0.0F);
    }
  }

  // Match the bar: while slid away use the edge strip (or full surface when pinned/hovered).
  // Never hit-test the slide-translated panel — that goes off-surface when smart_auto_hide
  // flips to pinned before hideOpacity recovers (dock rebuild on a new window).
  void
  syncDockAutoHideInputRegion(DockInstance& instance, const DockConfig& cfg, const DockPanelGeometry& panelGeometry) {
    if (instance.surface == nullptr) {
      return;
    }
    const int surfW = static_cast<int>(instance.surface->width());
    const int surfH = static_cast<int>(instance.surface->height());
    if (surfW <= 0 || surfH <= 0) {
      return;
    }

    if (!dockUsesAnyAutoHide(cfg)) {
      instance.surface->setInputRegion(
          shell::dock::computeInputRegion(cfg, panelGeometry, surfW, surfH, false, instance.fractionalScale)
      );
      return;
    }

    const bool fullSurface = instance.pointerInside || (cfg.smartAutoHide && instance.smartAutoHidePinnedVisible);
    if (fullSurface) {
      instance.surface->setInputRegion({InputRect{0, 0, surfW, surfH}});
      return;
    }
    instance.surface->setInputRegion(
        shell::dock::computeInputRegion(cfg, DockPanelGeometry{}, surfW, surfH, true, instance.fractionalScale)
    );
  }

  // The hide spring calls this every frame; send only a changed region.
  void sendDockBlur(DockInstance& instance, std::vector<InputRect> region) {
    if (instance.sentBlur == region) {
      return;
    }
    if (region.empty()) {
      instance.surface->clearBlurRegion();
    } else {
      instance.surface->setBlurRegion(region);
    }
    instance.sentBlur = std::move(region);
  }

  // The hide spring calls this every frame; send only a changed clip. An empty clip hides the shape.
  void setDockShapeClip(DockInstance& instance, const DockConfig& cfg, const DskSurfaceClient::Value& clip) {
    if (instance.shapeClip == clip) {
      return;
    }
    instance.shapeClip = clip;
    instance.shape->set(DskSurfaceClient::Property::Clip, clip);
    const auto concave = dockConcaveShape(cfg);
    // One radius per shape: the convex corners'. The concave corners face the screen edge.
    int edge = 0;
    double flare = 0.0;
    double radius = concave.radii.tl;
    if (concave.corners.bl == CornerShape::Concave && concave.corners.br == CornerShape::Concave) {
      edge = 3;
      flare = concave.logicalInset.left;
    } else if (concave.corners.tl == CornerShape::Concave && concave.corners.tr == CornerShape::Concave) {
      edge = 1;
      flare = concave.logicalInset.left;
      radius = concave.radii.bl;
    } else if (concave.corners.tl == CornerShape::Concave && concave.corners.bl == CornerShape::Concave) {
      edge = 4;
      flare = concave.logicalInset.top;
      radius = concave.radii.tr;
    } else if (concave.corners.tr == CornerShape::Concave && concave.corners.br == CornerShape::Concave) {
      edge = 2;
      flare = concave.logicalInset.top;
    }
    instance.shape->set(DskSurfaceClient::Property::Radius, {std::min({radius, clip[2] * 0.5, clip[3] * 0.5}), 0.0, 0.0, 0.0});
    instance.shape->set(DskSurfaceClient::Property::Join, {static_cast<double>(edge), flare, 0.0, 0.0});
  }

  void applyDockCompositorBlur(DockInstance& instance, const DockConfig& cfg) {
    if (instance.surface == nullptr) {
      return;
    }
    // Compositor blur is independent of scene opacity — clear it while auto-hide
    // has faded the dock out so a transparent buffer does not leave a blur halo.
    constexpr float kBlurVisibleOpacity = 0.02F;
    const bool hidden = !cfg.compositorBlur || (dockUsesAnyAutoHide(cfg) && instance.hideOpacity < kBlurVisibleOpacity);
    if (hidden) {
      if (instance.shape != nullptr) {
        setDockShapeClip(instance, cfg, {});
      } else {
        sendDockBlur(instance, {});
      }
      return;
    }
    if (instance.panel == nullptr) {
      return;
    }
    const auto concave = dockConcaveShape(cfg);
    float absX = 0.0F;
    float absY = 0.0F;
    Node::absolutePosition(instance.panel, absX, absY);

    const float insetL = concave.logicalInset.left;
    const float insetT = concave.logicalInset.top;
    const float insetR = concave.logicalInset.right;
    const float insetB = concave.logicalInset.bottom;
    const int px = static_cast<int>(std::lround(absX + insetL));
    const int py = static_cast<int>(std::lround(absY + insetT));
    const int pw = static_cast<int>(std::lround(instance.panel->width() - insetL - insetR));
    const int ph = static_cast<int>(std::lround(instance.panel->height() - insetT - insetB));
    if (instance.shape != nullptr) {
      setDockShapeClip(
          instance, cfg,
          {static_cast<double>(px), static_cast<double>(py), static_cast<double>(pw), static_cast<double>(ph)}
      );
      return;
    }
    sendDockBlur(instance, Surface::tessellateShape(px, py, pw, ph, concave.corners, concave.logicalInset, concave.radii));
  }

  void applyPanelPalette(DockInstance& instance, const DockConfig& cfg);

  void buildScene(DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks) {
    uiAssertNotRendering("shell::dock::buildScene");
    if (instance.surface == nullptr) {
      return;
    }
    if (instance.dsk == nullptr && instance.surface->wlSurface() != nullptr) {
      if (auto* manager = deps.platform.wayland().dskSurfaceManager(); manager != nullptr) {
        instance.dsk = std::make_unique<DskSurfaceClient>(manager, instance.surface->wlSurface());
      }
    }
    if (instance.dsk != nullptr && instance.dsk->hasShapes() && instance.shape == nullptr) {
      instance.shape = std::make_unique<DskShapeClient>(*instance.dsk);
    }

    Renderer& renderer = instance.surface->renderTarget().renderer();

    const auto& cfg = instance.config;
    const bool vert = shell::dock::isVerticalEdge(cfg.position);

    const auto w = static_cast<float>(instance.surface->width());
    const auto h = static_cast<float>(instance.surface->height());

    const auto& shadowConfig = deps.config.config().shell.shadow;
    const auto panelGeometry = shell::dock::computePanelGeometry(cfg, shadowConfig, w, h);
    const auto concave = shell::dock::dockConcaveShape(cfg);
    if (instance.shape != nullptr) {
      // Level 1 = the float plane (tokens elevation.e2), as the bar islands.
      const bool shadow = ui::surface_shadow::enabled(cfg.shadow, shadowConfig);
      instance.shape->set(DskSurfaceClient::Property::Elevation, {shadow ? 1.0 : 0.0, 0.0, 0.0, 0.0});
    }

    if (instance.sceneRoot == nullptr) {
      instance.sceneRoot = ui::node({});
      instance.sceneRoot->setAnimationManager(&instance.animations);
      instance.sceneRoot->setSize(w, h);
      instance.sceneRoot->setOpacity(1.0F);

      auto slide = ui::node({});
      slide->setParticipatesInLayout(false);
      instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

      // Shadow
      // A dock shape gets its shadow from the compositor (elevation).
      if (instance.shape == nullptr && ui::surface_shadow::enabled(cfg.shadow, shadowConfig)) {
        instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      }

      // Panel background (icons render as a sibling so magnification can extend past the capsule).
      instance.panel = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [concave](Box& box) {
                box.setCornerShapes(concave.corners);
                box.setLogicalInset(concave.logicalInset);
                box.setRadii(concave.radii);
                box.setClipChildren(false);
              },
          })
      ));
      instance.panel->setZIndex(0);

      // Clip overflow along the dock while retaining the cross-axis surface room used by magnification.
      auto viewport = ui::inputArea({});
      viewport->setClipChildren(true);
      viewport->setOnAxisHandler([inst = &instance](const InputArea::PointerData& data) {
        if (inst->row == nullptr || inst->maxScrollOffset <= 0.0F) {
          return false;
        }
        const auto& dockConfig = inst->config;
        const bool vertical = shell::dock::isVerticalEdge(dockConfig.position);
        const bool mainAxis =
            data.axis == (vertical ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        const bool verticalWheelOnHorizontalDock = !vertical && data.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
        if (!mainAxis && !verticalWheelOnHorizontalDock) {
          return false;
        }

        inst->scrollOffset += data.scrollDelta(Style::scrollWheelStep);
        applyDockScrollOffset(*inst, dockConfig);
        inst->inputDispatcher.syncPointerHover();

        float viewportX = 0.0F;
        float viewportY = 0.0F;
        Node::absolutePosition(inst->viewport, viewportX, viewportY);
        (void)shell::dock::syncHoverPointerFromScene(
            *inst, dockConfig, viewportX + data.localX, viewportY + data.localY
        );
        if (inst->surface != nullptr) {
          inst->surface->requestFrameTick();
          inst->surface->requestRedraw();
        }
        return true;
      });
      instance.viewport = static_cast<InputArea*>(instance.slideRoot->addChild(std::move(viewport)));
      instance.row = static_cast<Flex*>(instance.viewport->addChild(makeDockItemRow(cfg, vert)));
      instance.row->setZIndex(1);

      // Wire up InputDispatcher.
      instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
      instance.inputDispatcher.setCursorShapeCallback([&platform =
                                                           deps.platform](std::uint32_t serial, std::uint32_t shape) {
        platform.setCursorShape(serial, shape);
      });
      instance.inputDispatcher.setHoverChangeCallback([inst = &instance](InputArea* /*old*/, InputArea* next) {
        TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
      });

      // Populate items and wire up palette reactivity.
      if (callbacks.rebuildItems) {
        callbacks.rebuildItems(instance);
      }

      if (dockUsesAnyAutoHide(cfg)) {
        instance.smartAutoHidePinnedVisible =
            cfg.smartAutoHide && smartAutoHideWantsPinnedVisible(deps.platform, instance.output);
        instance.slideRoot->setOpacity(1.0F);
        const bool startHidden = cfg.smartAutoHide ? !instance.smartAutoHidePinnedVisible : cfg.autoHide;
        instance.hideOpacity = startHidden ? 0.0F : 1.0F;
      } else {
        instance.slideRoot->setOpacity(0.0F);
        instance.hideOpacity = 1.0F;
        instance.animations.animateSpatial(
            0.0F, 1.0F, {.damping = Tokens::springFade.damping, .stiffness = Tokens::springFade.stiffness},
            [slide = instance.slideRoot](float v) { slide->setOpacity(v); }, {}, instance.slideRoot
        );
      }

      instance.surface->setSceneRoot(instance.sceneRoot.get());
    } else {
      // Update corner shapes/radii/inset on reconfigure.
      instance.panel->setCornerShapes(concave.corners);
      instance.panel->setLogicalInset(concave.logicalInset);
      instance.panel->setRadii(concave.radii);
    }

    // Update root size on reconfigure.
    instance.sceneRoot->setSize(w, h);
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setSize(w, h);
    }

    // Shadow follows the same shape as the background
    if (instance.shadow != nullptr) {
      const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
      const auto shadowOffsetX = static_cast<float>(shadowOff.x);
      const auto shadowOffsetY = static_cast<float>(shadowOff.y);
      const RoundedRectStyle shadowStyle = ui::surface_shadow::style(
          shadowConfig, Style::glassTint(glass::Kind::Shape, cfg.backgroundOpacity),
          ui::surface_shadow::Shape{
              .corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii
          }
      );
      instance.shadow->setStyle(shadowStyle);
      instance.shadow->setZIndex(-1);
      instance.shadow->setPosition(
          panelGeometry.panelX - concave.logicalInset.left + shadowOffsetX,
          panelGeometry.panelY - concave.logicalInset.top + shadowOffsetY
      );
      instance.shadow->setSize(
          panelGeometry.panelW + concave.logicalInset.left + concave.logicalInset.right,
          panelGeometry.panelH + concave.logicalInset.top + concave.logicalInset.bottom
      );
    }

    // Panel
    applyPanelPalette(instance, cfg);
    instance.panel->setPosition(
        panelGeometry.panelX - concave.logicalInset.left, panelGeometry.panelY - concave.logicalInset.top
    );
    instance.panel->setSize(
        panelGeometry.panelW + concave.logicalInset.left + concave.logicalInset.right,
        panelGeometry.panelH + concave.logicalInset.top + concave.logicalInset.bottom
    );

    // The viewport follows the visible pill on its main axis and spans the surface on its cross axis,
    // allowing magnified icons to grow away from the screen edge without escaping past the pill ends.
    const auto contentLength = static_cast<float>(
        shell::dock::dockContentSize(cfg, instance.items.size() + shell::dock::dockLauncherButtonCount(cfg),
        instance.snapshot.hasSeparator())
    );
    if (vert) {
      instance.viewport->setPosition(0.0F, panelGeometry.panelY);
      instance.viewport->setSize(w, panelGeometry.panelH);
      instance.row->setPosition(panelGeometry.panelX, 0.0F);
      instance.row->setSize(panelGeometry.panelW, contentLength);
      instance.maxScrollOffset = std::max(0.0F, contentLength - panelGeometry.panelH);
    } else {
      instance.viewport->setPosition(panelGeometry.panelX, 0.0F);
      instance.viewport->setSize(panelGeometry.panelW, h);
      instance.row->setPosition(0.0F, panelGeometry.panelY);
      instance.row->setSize(contentLength, panelGeometry.panelH);
      instance.maxScrollOffset = std::max(0.0F, contentLength - panelGeometry.panelW);
    }
    applyDockScrollOffset(instance, cfg);
    instance.row->layout(renderer);
    shell::dock::syncDockItemRestPositions(instance, cfg);

    if (dockUsesAnyAutoHide(cfg)) {
      const auto hiddenDelta = shell::dock::computeHiddenSlideDelta(cfg, shadowConfig, w, h, panelGeometry);
      instance.slideHiddenDx = hiddenDelta.first;
      instance.slideHiddenDy = hiddenDelta.second;
    } else {
      instance.slideHiddenDx = 0.0F;
      instance.slideHiddenDy = 0.0F;
    }
    syncDockSlideLayerTransform(instance, cfg);
    syncDockAutoHideInputRegion(instance, cfg, panelGeometry);

    applyDockCompositorBlur(instance, cfg);

    // Palette reactivity.
    instance.paletteConn = paletteChanged().connect([inst = &instance] {
      applyPanelPalette(*inst, inst->config);
      if (inst->surface)
        inst->surface->requestRedraw();
    });

    if (callbacks.updateVisuals) {
      callbacks.updateVisuals(instance);
    }
  }

  void applyPanelPalette(DockInstance& instance, const DockConfig& cfg) {
    if (instance.panel == nullptr)
      return;
    // A small shape: liquid glass with the launcher pill's clear tint.
    const bool liquid = instance.dsk != nullptr && instance.dsk->hasLiquidProperty() && !Style::reduceTransparency();
    bool compositorTint = false;
    if (instance.shape != nullptr) {
      // A surface with shapes draws no surface glass: the material goes on the shape.
      const glass::Material material = Style::glassMaterial(glass::Kind::Shape);
      compositorTint = liquid && cfg.compositorBlur && Style::compositorDrawsTint();
      instance.shape->set(DskSurfaceClient::Property::Liquid, liquid ? glassLiquid(material) : DskSurfaceClient::Value{});
      instance.shape->set(
          DskSurfaceClient::Property::Tint, glassTintValue(materialRole(), compositorTint ? material.tint : 0.0F)
      );
    } else if (instance.dsk != nullptr && instance.dsk->hasLiquidProperty()) {
      const glass::Material material = Style::glassMaterial(glass::Kind::Shape);
      instance.dsk->set(DskSurfaceClient::Property::Liquid, liquid ? glassLiquid(material) : DskSurfaceClient::Value{});
      if (instance.dsk->hasTintProperty()) {
        compositorTint = liquid && cfg.compositorBlur && Style::compositorDrawsTint();
        instance.dsk->set(
            DskSurfaceClient::Property::Tint, glassTintValue(materialRole(), compositorTint ? material.tint : 0.0F)
        );
      }
    }
    // The compositor draws the tint (dsk v9): the dock paints no material fill.
    const float opacity = compositorTint ? 0.0F
        : liquid                         ? Style::glassMaterial(glass::Kind::Shape).tint
                                         : Style::glassTint(glass::Kind::Shape, cfg.backgroundOpacity);
    instance.panel->setFill(colorSpecFromRole(materialRole(), opacity));
    instance.panel->setBorder(cfg.border, cfg.borderWidth);
  }

  void resizeSurface(DockInstance& instance, const DockConfig& cfg, const ShellConfig::ShadowConfig& shadowConfig) {
    if (instance.surface == nullptr) {
      return;
    }

    const auto surfaceGeometry = shell::dock::computeSurfaceGeometry(
        cfg, shadowConfig, instance.items.size() + shell::dock::dockLauncherButtonCount(cfg), instance.fractionalScale,
        instance.outputLogicalWidth, instance.outputLogicalHeight, instance.snapshot.hasSeparator()
    );

    if (instance.surface->width() != surfaceGeometry.surfaceW
        || instance.surface->height() != surfaceGeometry.surfaceH) {
      instance.surface->requestSize(surfaceGeometry.surfaceW, surfaceGeometry.surfaceH);
    }
  }

  void animateDockHide(DockInstance& inst, float target) {
    const desk::motion::SpringConfig spring = target > 0.5F ? dockShowSpring() : dockHideSpring();
    if (inst.hideAnimId != 0 && inst.animations.retarget(inst.hideAnimId, target, spring)) {
      return;
    }
    if (inst.hideAnimId != 0) {
      inst.animations.cancel(inst.hideAnimId);
    }
    inst.hideAnimId = inst.animations.animateSpatial(
        inst.hideOpacity, target, spring,
        [&inst](float v) {
          inst.hideOpacity = std::clamp(v, 0.0F, 1.0F);
          syncDockSlideLayerTransform(inst, inst.config);
          applyDockCompositorBlur(inst, inst.config);
        },
        [&inst] { inst.hideAnimId = 0; }
    );
  }

  void revealAutoHideDock(DockInstance& inst) {
    const auto& cfg = inst.config;
    if (!dockUsesAnyAutoHide(cfg) || inst.surface == nullptr || inst.slideRoot == nullptr) {
      return;
    }

    constexpr float kSettledThreshold = 0.999F;
    const float current = inst.hideOpacity;
    if (current >= kSettledThreshold && inst.hideAnimId == 0) {
      syncDockAutoHideInputRegion(inst, cfg, DockPanelGeometry{});
      inst.surface->requestRedraw();
      return;
    }

    animateDockHide(inst, 1.0F);
    syncDockAutoHideInputRegion(inst, cfg, DockPanelGeometry{});
    inst.surface->requestRedraw();
  }

  void startHideFadeOut(DockInstance& inst) {
    // xdg tooltips are not parent-transformed with the slide; destroy immediately
    // so they cannot remain pinned after auto-hide starts (#4177).
    TooltipManager::instance().forceDestroy();
    animateDockHide(inst, 0.0F);
    syncDockAutoHideInputRegion(inst, inst.config, DockPanelGeometry{});
    if (inst.surface) {
      inst.surface->requestRedraw();
    }
  }

} // namespace shell::dock
