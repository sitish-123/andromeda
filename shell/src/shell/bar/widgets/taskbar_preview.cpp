#include "shell/bar/widgets/taskbar_preview.h"

#include "capture/toplevel_thumbnail_capture.h"
#include "compositors/compositor_platform.h"
#include "compositors/shell_wayland_connection.h"
#include "ui/palette.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/image.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
  // R6, on the rulebook 4 grid: cards of a 24 header over a 200 wide snapshot (16:10 box), 8 apart inside 12 padding;
  // the lens pads each card by 8.
  constexpr int kPad = 12;
  constexpr int kGap = 8;
  constexpr int kCardPad = 8;
  constexpr int kHeader = 24;
  constexpr int kHeaderGap = 4;
  constexpr int kThumbW = 200;
  constexpr int kThumbH = 125;
  constexpr int kIcon = 16;
  constexpr int kThumbRound = 8;
  constexpr int kCardW = kThumbW + 2 * kCardPad;
  constexpr int kCardH = kCardPad + kHeader + kHeaderGap + kThumbH + kCardPad;
  constexpr int kMaxCards = 6;
  constexpr auto kRefresh = std::chrono::milliseconds(1000);
} // namespace

TaskbarPreview::TaskbarPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext)
    : m_platform(platform), m_config(config), m_renderContext(renderContext),
      m_capture(std::make_unique<ToplevelThumbnailCapture>(platform.wayland())) {}

TaskbarPreview::~TaskbarPreview() { close(); }

void TaskbarPreview::open(Request request) {
  close();
  if (request.parent == nullptr || request.windows.empty()) {
    return;
  }
  m_request = std::move(request);
  m_cards.clear();
  for (const ToplevelInfo& window : m_request.windows) {
    if (m_cards.size() == kMaxCards) {
      break;
    }
    m_cards.push_back({.window = window});
  }
  const float s = m_request.contentScale;
  const auto n = static_cast<int>(m_cards.size());
  const float width = std::round(static_cast<float>(2 * kPad + n * kCardW + (n - 1) * kGap) * s);
  const float height = std::round(static_cast<float>(2 * kPad + kCardH) * s);
  m_chrome = popup_chrome::computeGeometry(width, height, m_config.config().shell.shadow, Style::popupShadowsEnabled());

  const std::string& bar = m_request.barPosition;
  const auto gap = static_cast<std::int32_t>(std::lround(Style::spaceSm * s));
  std::uint32_t anchor = XDG_POSITIONER_ANCHOR_TOP;
  std::uint32_t gravity = XDG_POSITIONER_GRAVITY_TOP;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = -gap;
  popup_chrome::Attachment attachment{
      .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Bottom
  };
  if (bar == "top") {
    anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
    gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
    offsetY = gap;
    attachment.vertical = popup_chrome::VerticalAttachment::Top;
  } else if (bar == "left") {
    anchor = XDG_POSITIONER_ANCHOR_RIGHT;
    gravity = XDG_POSITIONER_GRAVITY_RIGHT;
    offsetX = gap;
    offsetY = 0;
    attachment = {.horizontal = popup_chrome::HorizontalAttachment::Left,
                  .vertical = popup_chrome::VerticalAttachment::Center};
  } else if (bar == "right") {
    anchor = XDG_POSITIONER_ANCHOR_LEFT;
    gravity = XDG_POSITIONER_GRAVITY_LEFT;
    offsetX = -gap;
    offsetY = 0;
    attachment = {.horizontal = popup_chrome::HorizontalAttachment::Right,
                  .vertical = popup_chrome::VerticalAttachment::Center};
  }
  PopupSurfaceConfig config{
      .anchorX = m_request.anchor.x,
      .anchorY = m_request.anchor.y,
      .anchorWidth = std::max(1, m_request.anchor.width),
      .anchorHeight = std::max(1, m_request.anchor.height),
      .width = m_chrome.surfaceWidth,
      .height = m_chrome.surfaceHeight,
      .anchor = anchor,
      .gravity = gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = offsetX,
      .offsetY = offsetY,
      .serial = m_platform.lastInputSerial(),
      .grab = false,
  };
  popup_chrome::applyToConfig(config, m_chrome, attachment);

  m_surface = std::make_unique<PopupSurface>(m_platform.wayland());
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool, bool needsLayout) {
    if (m_surface == nullptr || m_surface->width() == 0 || m_surface->height() == 0) {
      return;
    }
    m_renderContext.makeCurrent(m_surface->renderTarget());
    if (m_sceneRoot == nullptr || needsLayout || m_dirty) {
      build();
    }
  });
  m_surface->setDismissedCallback([this] { DeferredCall::callLater([this] { close(); }); });
  if (!m_surface->initialize(m_request.parent, m_request.output, config)) {
    m_surface.reset();
    return;
  }
  popup_chrome::setContentInputRegion(*m_surface, m_chrome);
  m_nextCapture = 0;
  captureNext();
}

void TaskbarPreview::close() {
  m_refresh.stop();
  m_capture->cancelInFlight();
  m_input.setSceneRoot(nullptr);
  m_surface.reset();
  m_sceneRoot.reset();
  m_cards.clear();
  m_pointerInside = false;
  m_hasHover = false;
  m_request.key.clear();
}

void TaskbarPreview::captureNext() {
  if (!isOpen() || m_capture->busy()) {
    return;
  }
  while (m_nextCapture < m_cards.size()) {
    const ToplevelInfo& window = m_cards[m_nextCapture].window;
    ext_foreign_toplevel_handle_v1* handle = window.extHandle;
    const std::size_t index = m_nextCapture++;
    // A minimized window has no fresh frame; its card keeps the icon.
    bool live = false;
    m_platform.wayland().visitExtToplevelHandles([&](ext_foreign_toplevel_handle_v1* h) { live |= h == handle; });
    if (handle == nullptr || !live
        || (window.minimizeState.has_value() && window.minimizeState->minimized)) {
      continue;
    }
    const auto s = m_request.contentScale;
    m_capture->capture(
        handle, static_cast<int>(std::lround(kThumbW * s)), static_cast<int>(std::lround(kThumbH * s)),
        [this, index](std::optional<ScreencopyImage> image, const std::string& error) {
          if (isOpen() && error.empty() && image.has_value() && index < m_cards.size()) {
            m_cards[index].snapshot = std::make_shared<ScreencopyImage>(std::move(*image));
            requestRebuild();
          }
          captureNext();
        }
    );
    return;
  }
  // Every card has a snapshot: refresh them while the popover stays open.
  m_refresh.start(kRefresh, [this] {
    m_nextCapture = 0;
    captureNext();
  });
}

void TaskbarPreview::requestRebuild() {
  m_dirty = true;
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void TaskbarPreview::build() {
  UiPhaseScope layoutPhase(UiPhase::Layout);
  m_dirty = false;
  Renderer& renderer = m_surface->renderTarget().renderer();
  const float s = m_request.contentScale;
  m_sceneRoot = ui::node({});
  m_sceneRoot->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
  if (Style::popupShadowsEnabled()) {
    (void)popup_chrome::addShadow(*m_sceneRoot, m_chrome, m_config.config().shell.shadow, Style::scaledRadiusLg(s));
  }
  (void)popup_chrome::addCardBackground(*m_sceneRoot, m_chrome, s);
  popup_chrome::setCardBlurRegion(*m_surface, m_chrome, s);

  Box* lens = nullptr;
  m_sceneRoot->addChild(ui::box({
      .out = &lens,
      .fill = colorSpecFromRole(ColorRole::Lens),
      .radius = static_cast<float>(kThumbRound + kCardPad) * s,
      .width = kCardW * s,
      .height = kCardH * s,
      .visible = m_hasHover,
      .participatesInLayout = false,
  }));
  const float originX = m_chrome.contentX() + kPad * s;
  const float originY = m_chrome.contentY() + kPad * s;
  const auto cardX = [&](std::size_t i) { return std::round(originX + static_cast<float>(i) * (kCardW + kGap) * s); };
  if (m_hasHover) {
    lens->setPosition(cardX(m_hovered), originY);
  }

  for (std::size_t i = 0; i < m_cards.size(); ++i) {
    const Card& card = m_cards[i];
    const float x = cardX(i);
    InputArea* area = nullptr;
    m_sceneRoot->addChild(ui::inputArea({.out = &area, .x = x, .y = originY, .width = kCardW * s, .height = kCardH * s}));
    Button* closeButton = nullptr;

    // Header: icon 16, title (footnote 500, label_1), close 24 at the trailing edge on hover.
    const float headerY = originY + kCardPad * s;
    float titleX = x + kCardPad * s;
    if (!m_request.icon.path.empty()) {
      auto icon = ui::image({.fit = ImageFit::Contain, .width = kIcon * s, .height = kIcon * s, .participatesInLayout = false});
      if (icon->setSourceFile(renderer, m_request.icon.path, static_cast<int>(std::lround(kIcon * s)), true)) {
        icon->setPosition(titleX, std::round(headerY + (kHeader - kIcon) * 0.5F * s));
        m_sceneRoot->addChild(std::move(icon));
        titleX += (kIcon + Style::spaceSm) * s;
      }
    }
    const float closeX = x + (kCardW - kCardPad - kHeader) * s;
    auto title = ui::label({
        .text = card.window.title.empty() ? card.window.appId : card.window.title,
        .fontSize = Style::fontSizeCaption * s,
        .fontWeight = FontWeight::Medium,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxWidth = std::max(1.0F, closeX - titleX - Style::spaceXs * s),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .participatesInLayout = false,
    });
    title->measure(renderer);
    title->setPosition(titleX, std::round(headerY + (kHeader * s - title->height()) * 0.5F));
    m_sceneRoot->addChild(std::move(title));

    // Snapshot box: Fill 2 until the first capture lands, then the window at its own aspect inside it.
    const float thumbY = headerY + (kHeader + kHeaderGap) * s;
    auto thumbBox = ui::box({
        .fill = colorSpecFromRole(ColorRole::Fill2),
        .radius = kThumbRound * s,
        .width = kThumbW * s,
        .height = kThumbH * s,
        .participatesInLayout = false,
    });
    thumbBox->setPosition(x + kCardPad * s, thumbY);
    if (card.snapshot != nullptr && card.snapshot->width > 0 && !card.snapshot->rgba.empty()) {
      auto image = ui::image({
          .fit = ImageFit::Contain,
          .radius = kThumbRound * s,
          .width = kThumbW * s,
          .height = kThumbH * s,
          .participatesInLayout = false,
      });
      if (image->setSourceRaw(
              renderer, card.snapshot->rgba.data(), card.snapshot->rgba.size(), card.snapshot->width,
              card.snapshot->height, card.snapshot->width * 4, PixmapFormat::RGBA, true
          )) {
        thumbBox->addChild(std::move(image));
      }
    }
    m_sceneRoot->addChild(std::move(thumbBox));

    const bool hovered = m_hasHover && m_hovered == i;
    m_sceneRoot->addChild(ui::button({
        .out = &closeButton,
        .glyph = "close",
        .glyphSize = Style::fontSizeCaption * s,
        .controlHeight = kHeader * s,
        .variant = ButtonVariant::Ghost,
        .padding = 0.0F,
        .radius = kHeader * 0.5F * s,
        .width = kHeader * s,
        .height = kHeader * s,
        .visible = hovered,
        .participatesInLayout = false,
        .onClick = [this, window = card.window] {
          m_platform.closeToplevelInfo(window);
          DeferredCall::callLater([this] { close(); });
        },
    }));
    closeButton->setPosition(closeX, headerY);

    area->setOnEnter([this, i](const InputArea::PointerData&) {
      if (!m_hasHover || m_hovered != i) {
        m_hasHover = true;
        m_hovered = i;
        requestRebuild();
      }
    });
    area->setOnClick([this, window = card.window](const InputArea::PointerData&) {
      m_platform.activateToplevelInfo(window);
      DeferredCall::callLater([this] { close(); });
    });
  }

  for (const auto& child : m_sceneRoot->children()) {
    child->layout(renderer);
  }
  m_input.setSceneRoot(m_sceneRoot.get());
  m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_platform.setCursorShape(serial, shape);
  });
  m_surface->setSceneRoot(m_sceneRoot.get());
}

bool TaskbarPreview::onPointerEvent(const PointerEvent& event) {
  if (m_surface == nullptr) {
    return false;
  }
  const bool onPopup = event.surface != nullptr && event.surface == m_surface->wlSurface();
  const auto leave = [this] {
    if (!m_pointerInside) {
      return;
    }
    m_pointerInside = false;
    m_input.pointerLeave();
    if (m_hasHover) {
      m_hasHover = false;
      requestRebuild();
    }
    if (m_onPointerLeave) {
      m_onPointerLeave();
    }
  };
  switch (event.type) {
  case PointerEvent::Type::Enter:
  case PointerEvent::Type::Motion:
    if (onPopup) {
      m_pointerInside = true;
      m_input.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      return true;
    }
    if (event.surface != nullptr) {
      leave();
    }
    return false;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      leave();
    }
    return false;
  case PointerEvent::Type::Button:
    if (!onPopup) {
      return false;
    }
    m_input.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    m_input.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial,
        event.time, event.touch
    );
    return true;
  case PointerEvent::Type::Axis:
    return onPopup;
  }
  return false;
}
