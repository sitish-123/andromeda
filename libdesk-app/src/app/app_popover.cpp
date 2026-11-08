#include "app/app_popover.h"

#include "render/scene/node.h"
#include "ui/popup_parent.h"
#include "wayland/popup_surface.h"

#include <xdg-shell-client-protocol.h>

AppPopover::AppPopover(WaylandConnection& wayland, RenderContext& renderContext) {
  initializeBase(wayland, m_settings, renderContext);
}

AppPopover::~AppPopover() { destroyPopup(); }

void AppPopover::open(Request request) {
  if (request.parentXdg == nullptr || request.parentWl == nullptr || !request.content) {
    return;
  }
  m_request = std::move(request);
  m_settings.scale = m_request.scale;
  const PopupSurfaceConfig config = popupConfig();
  if (!openPopupAsChild(config, XdgPopupParent{.xdgSurface = m_request.parentXdg, .wlSurface = m_request.parentWl,
                                               .output = nullptr, .serial = m_request.serial, .width = 0, .height = 0})) {
    destroyPopup();
  }
}

void AppPopover::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  m_parent = contentParent;
  m_body = contentParent->addChild(m_request.content(m_request.scale));
}

void AppPopover::layoutSheet(float contentWidth, float contentHeight) {
  if (m_body != nullptr) {
    m_body->setSize(contentWidth, contentHeight);
    m_body->layout(renderer());
  }
}

PopupSurfaceConfig AppPopover::popupConfig() const {
  const auto width = static_cast<std::uint32_t>(std::max(1.0F, m_request.width));
  const auto height = static_cast<std::uint32_t>(std::max(1.0F, m_request.height));
  // Under the control, its right edges lined up; slides back on screen near the edges.
  return PopupSurfaceConfig{
      .anchorX = m_request.anchorX,
      .anchorY = m_request.anchorY,
      .anchorWidth = std::max(1, m_request.anchorWidth),
      .anchorHeight = std::max(1, m_request.anchorHeight),
      .width = width,
      .height = height,
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = 0,
      .offsetY = 4,
      .serial = m_request.serial,
      .grab = true,
  };
}

void AppPopover::resize(std::int32_t anchorX, std::int32_t anchorY, std::int32_t anchorWidth, std::int32_t anchorHeight,
                        float width, float height) {
  if (!isOpen()) {
    return;
  }
  m_request.anchorX = anchorX;
  m_request.anchorY = anchorY;
  m_request.anchorWidth = anchorWidth;
  m_request.anchorHeight = anchorHeight;
  m_request.width = width;
  m_request.height = height;
  resizePopup(popupConfig());
}

void AppPopover::refresh() {
  if (!isOpen() || m_parent == nullptr) {
    return;
  }
  if (m_body != nullptr) {
    m_parent->removeChild(m_body);
  }
  m_body = m_parent->addChild(m_request.content(m_request.scale));
  requestLayout();
}

void AppPopover::onSheetClose() {
  m_parent = nullptr;
  m_body = nullptr;
  if (m_closed) {
    m_closed();
  }
}
