#include "shell/bar/widgets/taskbar_preview.h"

#include "capture/toplevel_thumbnail_capture.h"
#include "compositors/compositor_platform.h"
#include "compositors/shell_wayland_connection.h"
#include "ui/palette.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "render/animation/motion_service.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
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
  // Board desktop-layouts.html section 2 (radius set B): popover p 8; cards 200 wide with p 4 (the lens, r 8), 8 apart,
  // a 24 header 8 above a 124 high snapshot at r 4.
  constexpr int kPad = 8;
  constexpr int kGap = 8;
  constexpr int kCardPad = 4;
  constexpr int kHeader = 24;
  constexpr int kHeaderGap = 8;
  // 3 or more windows shrink the cards to 160 (board 2 refined).
  constexpr int kThumbW = 192;
  constexpr int kThumbH = 124;
  constexpr int kSmallThumbW = 152;
  constexpr int kSmallThumbH = 96;
  constexpr int kIcon = 16;
  constexpr int kThumbRound = 4;
  constexpr int kLensRound = 8;
  constexpr int kMaxCards = 6;
  // Group card: members 2 apart inside a 2 px inset, tiles r 2, member icons 4 apart; a separator 4 from each side.
  constexpr int kGroupInset = 2;
  constexpr int kTileRound = 2;
  constexpr int kGroupIconGap = 4;
  constexpr int kSepMargin = 4;
  constexpr int kSepExtra = 2 * kSepMargin + 1 + kGap;
  constexpr auto kRefresh = std::chrono::milliseconds(1000);
} // namespace

TaskbarPreview::TaskbarPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext)
    : m_platform(platform), m_config(config), m_renderContext(renderContext),
      m_capture(std::make_unique<ToplevelThumbnailCapture>(platform.wayland())) {}

TaskbarPreview::~TaskbarPreview() {
  *m_alive = false;
  closeNow();
}

void TaskbarPreview::defer(std::function<void()> fn) {
  DeferredCall::callLater([alive = std::weak_ptr<bool>(m_alive), fn = std::move(fn)] {
    if (const auto token = alive.lock(); token != nullptr && *token) {
      fn();
    }
  });
}

int TaskbarPreview::cardW() const { return m_thumbW + 2 * kCardPad; }

int TaskbarPreview::cardH() const { return kCardPad + kHeader + kHeaderGap + m_thumbH + kCardPad; }

float TaskbarPreview::slotX(std::size_t slot) const {
  const float s = m_request.contentScale;
  const float extra = hasGroup() && slot >= 1 ? kSepExtra * s : 0.0F;
  return std::round(m_chrome.contentX() + kPad * s + static_cast<float>(slot) * (cardW() + kGap) * s + extra);
}

void TaskbarPreview::open(Request request) {
  if (request.parent == nullptr || request.windows.empty()) {
    closeNow();
    return;
  }
  // Another bar or output: a new surface. The same bar: the shown (or closing) surface moves.
  const bool reuse = m_surface != nullptr && request.parent == m_request.parent && request.output == m_request.output;
  if (!reuse) {
    closeNow();
  }
  std::vector<Card> previous = std::move(m_cards);
  m_request = std::move(request);
  m_cards.clear();
  m_group.clear();
  for (auto& member : m_platform.snapGroupFor(m_request.windows)) {
    auto [icon, name] = m_request.describeApp ? m_request.describeApp(member.window.appId)
                                              : std::pair<ResolvedIcon, std::string>{{}, member.window.appId};
    m_group.push_back({.window = std::move(member.window), .zone = member.zone, .icon = std::move(icon), .name = std::move(name)});
  }
  for (const GroupMember& member : m_group) {
    m_cards.push_back({.window = member.window});
  }
  for (const ToplevelInfo& window : m_request.windows) {
    if (m_cards.size() == m_group.size() + kMaxCards) {
      break;
    }
    m_cards.push_back({.window = window});
  }
  for (Card& card : m_cards) {
    const auto kept = std::ranges::find_if(previous, [&](const Card& old) {
      return old.window.extHandle != nullptr && old.window.extHandle == card.window.extHandle;
    });
    if (kept != previous.end()) {
      card.snapshot = kept->snapshot;
    }
  }
  m_hasHover = false;
  const PopupSurfaceConfig config = surfaceConfig();
  if (reuse) {
    m_surface->resize(config.width, config.height, false);
    m_surface->repositionAnchor(config);
    popup_chrome::setContentInputRegion(*m_surface, m_chrome);
    requestRebuild();
  } else {
    m_surface = std::make_unique<PopupSurface>(m_platform.wayland());
    m_surface->setRenderContext(&m_renderContext);
    m_surface->setAnimationManager(&m_animations);
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
    m_surface->setDismissedCallback([this] { defer([this] { closeNow(); }); });
    if (!m_surface->initialize(m_request.parent, m_request.output, config)) {
      m_surface.reset();
      return;
    }
    popup_chrome::setContentInputRegion(*m_surface, m_chrome);
    // Grow (P10) from the button: the first frame is small and clear.
    m_scale = 0.92F;
    m_opacity = 0.0F;
  }
  m_refresh.stop();
  m_capture->cancelInFlight();
  m_nextCapture = 0;
  // Shown first: captureNext() skips a closing preview, and a reopen during the Return fade is still closing.
  driveShown(true);
  captureNext();
}

PopupSurfaceConfig TaskbarPreview::surfaceConfig() {
  const auto n = static_cast<int>(slotCount());
  const bool small = n >= 3;
  m_thumbW = small ? kSmallThumbW : kThumbW;
  m_thumbH = small ? kSmallThumbH : kThumbH;
  const float s = m_request.contentScale;
  const int sep = hasGroup() && n > 1 ? kSepExtra : 0;
  const float width = std::round(static_cast<float>(2 * kPad + n * cardW() + (n - 1) * kGap + sep) * s);
  const float height = std::round(static_cast<float>(2 * kPad + cardH()) * s);
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
  return config;
}

void TaskbarPreview::close() {
  if (m_surface == nullptr || m_closing) {
    return;
  }
  m_closing = true;
  m_refresh.stop();
  m_capture->cancelInFlight();
  m_pointerInside = false;
  m_request.key.clear();
  driveShown(false);
}

void TaskbarPreview::closeNow() {
  m_animations.cancelAll();
  m_scaleAnim = m_opacityAnim = m_lensLeadAnim = m_lensTrailAnim = m_lensFadeAnim = 0;
  m_closing = false;
  m_refresh.stop();
  m_capture->cancelInFlight();
  m_input.setSceneRoot(nullptr);
  m_lens = nullptr;
  m_surface.reset();
  m_sceneRoot.reset();
  m_cards.clear();
  m_group.clear();
  m_pointerInside = false;
  m_hasHover = false;
  m_request.key.clear();
}

// Grow: scale 0.92 -> 1 on open, opacity on fade. Return: scale -> 0.96 on close, opacity on fade_out. Reduced motion
// keeps the scale at 1 (spatial springs jump) and only fades.
void TaskbarPreview::driveShown(bool shown) {
  m_closing = !shown;
  m_scaleAnim = m_animations.drive(
      m_scaleAnim, m_scale, shown ? 1.0F : 0.96F, Style::spring(shown ? Tokens::springOpen : Tokens::springClose),
      [this](float v) {
        m_scale = v;
        applyTransform();
      },
      [this] { m_scaleAnim = 0; }, nullptr, 0.0F, Unit::Scale, true
  );
  m_opacityAnim = m_animations.drive(
      m_opacityAnim, m_opacity, shown ? 1.0F : 0.0F, Style::spring(shown ? Tokens::springFade : Tokens::springFadeOut),
      [this](float v) {
        m_opacity = v;
        applyTransform();
      },
      [this] {
        m_opacityAnim = 0;
        // Never from inside the surface's own frame tick.
        if (m_closing) {
          defer([this] {
            if (m_closing) {
              closeNow();
            }
          });
        }
      }
  );
  applyTransform();
}

void TaskbarPreview::applyTransform() {
  if (m_sceneRoot != nullptr) {
    m_sceneRoot->setScale(m_scale);
    m_sceneRoot->setOpacity(m_opacity);
  }
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

// Travel (P1): the edge toward the new slot leads on snap, the other trails on trail, so the lens stretches and settles.
// Reduced motion: both edges jump and the lens fades in at its new place.
void TaskbarPreview::moveLens(std::size_t slot) {
  const float s = m_request.contentScale;
  const float left = slotX(slot);
  const float right = left + cardW() * s;
  if (!m_hasHover) {
    m_animations.cancel(m_lensLeadAnim);
    m_animations.cancel(m_lensTrailAnim);
    m_lensLeadAnim = m_lensTrailAnim = 0;
    m_lensLeft = left;
    m_lensRight = right;
    applyLens();
    return;
  }
  const bool forward = left > m_lensLeft;
  const auto edge = [this](bool leftEdge) {
    return [this, leftEdge](float v) {
      (leftEdge ? m_lensLeft : m_lensRight) = v;
      applyLens();
    };
  };
  m_lensLeadAnim = m_animations.drive(
      m_lensLeadAnim, forward ? m_lensRight : m_lensLeft, forward ? right : left, Style::spring(Tokens::springSnap),
      edge(!forward), [this] { m_lensLeadAnim = 0; }, nullptr, 0.0F, Unit::Px, true
  );
  m_lensTrailAnim = m_animations.drive(
      m_lensTrailAnim, forward ? m_lensLeft : m_lensRight, forward ? left : right, Style::spring(Tokens::springTrail),
      edge(forward), [this] { m_lensTrailAnim = 0; }, nullptr, 0.0F, Unit::Px, true
  );
  if (MotionService::instance().reduced()) {
    m_lensOpacity = 0.0F;
    m_lensFadeAnim = m_animations.drive(
        m_lensFadeAnim, 0.0F, 1.0F, Style::spring(Tokens::springFade),
        [this](float v) {
          m_lensOpacity = v;
          applyLens();
        },
        [this] { m_lensFadeAnim = 0; }
    );
  }
  applyLens();
}

void TaskbarPreview::applyLens() {
  if (m_lens != nullptr) {
    m_lens->setPosition(std::round(m_lensLeft), m_lens->y());
    m_lens->setSize(std::max(1.0F, std::round(m_lensRight - m_lensLeft)), m_lens->height());
    m_lens->setOpacity(m_lensOpacity);
  }
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
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
        handle, static_cast<int>(std::lround(m_thumbW * s)), static_cast<int>(std::lround(m_thumbH * s)),
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
  // Every card has a snapshot: refresh them while the popover stays open. A snap group that formed or broke up since
  // the popover opened (it stays open over the same button) rebuilds the cards.
  m_refresh.start(kRefresh, [this] {
    const auto members = m_platform.snapGroupFor(m_request.windows);
    const bool same = members.size() == m_group.size()
        && std::ranges::equal(members, m_group, [](const auto& now, const GroupMember& shown) {
             return now.window.identifier == shown.window.identifier;
           });
    if (!same) {
      Request again = m_request;
      defer([this, again = std::move(again)]() mutable {
        if (isOpen()) {
          open(std::move(again));
        }
      });
      return;
    }
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
  // Grow and Return pivot on the card edge that faces the bar.
  {
    const std::string& bar = m_request.barPosition;
    const float left = m_chrome.contentX();
    const float top = m_chrome.contentY();
    const float w = m_chrome.contentWidth;
    const float h = m_chrome.contentHeight;
    m_sceneRoot->setTransformOrigin(
        bar == "left" ? left : bar == "right" ? left + w : left + w * 0.5F,
        bar == "top" ? top : (bar == "left" || bar == "right") ? top + h * 0.5F : top + h
    );
  }
  m_sceneRoot->setScale(m_scale);
  m_sceneRoot->setOpacity(m_opacity);
  if (Style::popupShadowsEnabled()) {
    (void)popup_chrome::addShadow(*m_sceneRoot, m_chrome, m_config.config().shell.shadow, Style::scaledRadiusLg(s));
  }
  (void)popup_chrome::addCardBackground(*m_sceneRoot, m_chrome, s);
  popup_chrome::setCardBlurRegion(*m_surface, m_chrome, s);

  m_sceneRoot->addChild(ui::box({
      .out = &m_lens,
      .fill = colorSpecFromRole(ColorRole::Lens),
      .radius = static_cast<float>(kLensRound) * s,
      .width = cardW() * s,
      .height = cardH() * s,
      .visible = m_hasHover,
      .participatesInLayout = false,
  }));
  const float originY = m_chrome.contentY() + kPad * s;
  m_lens->setPosition(m_lensLeft, originY);
  applyLens();
  const float headerY = originY + kCardPad * s;
  const float thumbY = headerY + (kHeader + kHeaderGap) * s;
  const auto addIcon = [&](const ResolvedIcon& resolved, float x) {
    if (resolved.path.empty()) {
      return false;
    }
    auto icon = ui::image({.fit = ImageFit::Contain, .width = kIcon * s, .height = kIcon * s, .participatesInLayout = false});
    if (!icon->setSourceFile(renderer, resolved.path, static_cast<int>(std::lround(kIcon * s)), true)) {
      return false;
    }
    icon->setPosition(x, std::round(headerY + (kHeader - kIcon) * 0.5F * s));
    m_sceneRoot->addChild(std::move(icon));
    return true;
  };
  const auto addTitle = [&](const std::string& text, float x, float maxRight) {
    auto title = ui::label({
        .text = text,
        .fontSize = Style::fontSizeCaption * s,
        .fontWeight = FontWeight::Medium,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxWidth = std::max(1.0F, maxRight - x),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .participatesInLayout = false,
    });
    title->measure(renderer);
    title->setPosition(x, std::round(headerY + (kHeader * s - title->height()) * 0.5F));
    m_sceneRoot->addChild(std::move(title));
  };
  // A snapshot covering a box of `w` x `h` under a 0.5 px edge hairline (board .th); Fill 2 until the first capture lands.
  const auto addSnapshot = [&](const Card& card, float x, float y, float w, float h, float radius, bool edged = true) {
    auto thumbBox = ui::box({
        .fill = colorSpecFromRole(ColorRole::Fill2),
        .radius = radius,
        .width = w,
        .height = h,
        .participatesInLayout = false,
    });
    thumbBox->setPosition(x, y);
    if (card.snapshot != nullptr && card.snapshot->width > 0 && !card.snapshot->rgba.empty()) {
      auto image = ui::image({
          .fit = ImageFit::Cover,
          .radius = radius,
          .width = w,
          .height = h,
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
    if (!edged) {
      return;
    }
    // tokens.css --edge has no palette role: white 75 % light, 12 % dark.
    auto edge = ui::box({
        .border = ColorSpec{.fixed = rgba(1.0F, 1.0F, 1.0F, isLightPalette() ? 0.749F : 0.122F)},
        .borderWidth = 0.5F * s,
        .radius = radius,
        .width = w,
        .height = h,
        .participatesInLayout = false,
    });
    edge->setPosition(x, y);
    m_sceneRoot->addChild(std::move(edge));
  };
  const auto addArea = [&](std::size_t slot, const ToplevelInfo& target) {
    InputArea* area = nullptr;
    m_sceneRoot->addChild(
        ui::inputArea({.out = &area, .x = slotX(slot), .y = originY, .width = cardW() * s, .height = cardH() * s})
    );
    area->setOnEnter([this, slot](const InputArea::PointerData&) {
      if (!m_hasHover || m_hovered != slot) {
        moveLens(slot);
        m_hasHover = true;
        m_hovered = slot;
        requestRebuild();
      }
    });
    // Activating one member restores and raises its whole snap group (compositor View::raiseToTop).
    area->setOnClick([this, target](const InputArea::PointerData&) {
      m_platform.activateToplevelInfo(target);
      defer([this] { close(); });
    });
  };

  if (hasGroup()) {
    // Group card: member icons and names over a mini of the real layout, one tile per member at its zone.
    const float x = slotX(0);
    // Focus lands on this app's member; the others come up with it.
    const auto own = std::ranges::find_if(m_group, [&](const GroupMember& member) {
      return !m_request.windows.empty() && member.window.appId == m_request.windows.front().appId;
    });
    addArea(0, own != m_group.end() ? own->window : m_group.front().window);
    float titleX = x + kCardPad * s;
    std::string names;
    for (const GroupMember& member : m_group) {
      if (addIcon(member.icon, titleX)) {
        titleX += (kIcon + kGroupIconGap) * s;
      }
      names += (names.empty() ? "" : " + ") + member.name;
    }
    addTitle(names, titleX + kGroupIconGap * s, x + (cardW() - kCardPad) * s);
    const float boxX = x + kCardPad * s;
    m_sceneRoot->addChild([&] {
      // The default wallpaper's night blue to teal, as the desktop layout gallery draws it (board .glay).
      const Color night = rgbHex(0x0D1630);
      const Color teal = rgbHex(0x14455A);
      auto board = std::make_unique<RectNode>();
      board->setParticipatesInLayout(false);
      board->setPosition(boxX, thumbY);
      board->setFrameSize(m_thumbW * s, m_thumbH * s);
      board->setStyle(RoundedRectStyle{
          .fill = night,
          .border = clearColor(),
          .fillMode = FillMode::LinearGradient,
          .gradientDirection = GradientDirection::Horizontal,
          .gradientStops = {GradientStop{0.0F, night}, GradientStop{0.55F, teal}, GradientStop{1.0F, night}},
          .radius = kThumbRound * s,
          .softness = 1.0F,
          .borderWidth = 0.0F,
      });
      return board;
    }());
    const float innerW = (m_thumbW - 2 * kGroupInset) * s;
    const float innerH = (m_thumbH - 2 * kGroupInset) * s;
    const float half = kGroupInset * 0.5F * s;
    for (std::size_t i = 0; i < m_group.size(); ++i) {
      const auto& zone = m_group[i].zone;
      const float tx = std::round(boxX + kGroupInset * s + zone[0] * innerW + (zone[0] > 0.0F ? half : 0.0F));
      const float ty = std::round(thumbY + kGroupInset * s + zone[1] * innerH + (zone[1] > 0.0F ? half : 0.0F));
      const float right = std::round(boxX + kGroupInset * s + (zone[0] + zone[2]) * innerW - (zone[0] + zone[2] < 1.0F ? half : 0.0F));
      const float bottom = std::round(thumbY + kGroupInset * s + (zone[1] + zone[3]) * innerH - (zone[1] + zone[3] < 1.0F ? half : 0.0F));
      addSnapshot(m_cards[i], tx, ty, std::max(1.0F, right - tx), std::max(1.0F, bottom - ty), kTileRound * s, false);
    }
    if (slotCount() > 1) {
      m_sceneRoot->addChild([&] {
        auto line = ui::box({
            .fill = colorSpecFromRole(ColorRole::Separator),
            .width = std::max(1.0F, std::round(s)),
            .height = cardH() * s,
            .participatesInLayout = false,
        });
        line->setPosition(std::round(x + (cardW() + kGap + kSepMargin) * s), originY);
        return line;
      }());
    }
  }

  for (std::size_t slot = hasGroup() ? 1 : 0; slot < slotCount(); ++slot) {
    const Card& card = m_cards[m_group.size() + slot - (hasGroup() ? 1 : 0)];
    const float x = slotX(slot);
    addArea(slot, card.window);

    // Header: icon 16, title (footnote 500, label_1), close 24 at the trailing edge on hover.
    float titleX = x + kCardPad * s;
    if (addIcon(m_request.icon, titleX)) {
      titleX += (kIcon + Style::spaceSm) * s;
    }
    const float closeX = x + (cardW() - kCardPad - kHeader) * s;
    addTitle(card.window.title.empty() ? card.window.appId : card.window.title, titleX, closeX - Style::spaceXs * s);
    addSnapshot(card, x + kCardPad * s, thumbY, m_thumbW * s, m_thumbH * s, kThumbRound * s);

    // Close: label_2 glyph; fill_2 on hover is replaced by danger with a white glyph (board 2).
    const Button::ButtonPalette closePalette{
        .normal = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label2)},
        .hover = {.bg = colorSpecFromRole(ColorRole::Danger), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::OnAccent)},
        .pressed = {.bg = colorSpecFromRole(ColorRole::Danger), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::OnAccent)},
        .disabled = {.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Label3)},
    };
    Button* closeButton = nullptr;
    m_sceneRoot->addChild(ui::button({
        .out = &closeButton,
        .glyph = "close",
        .glyphSize = Style::fontSizeCaption * s,
        .controlHeight = kHeader * s,
        .variant = ButtonVariant::Ghost,
        .customPalette = closePalette,
        .padding = 0.0F,
        .radius = kHeader * 0.5F * s,
        .width = kHeader * s,
        .height = kHeader * s,
        .visible = m_hasHover && m_hovered == slot,
        .participatesInLayout = false,
        .onClick = [this, window = card.window] {
          m_platform.closeToplevelInfo(window);
          defer([this] { close(); });
        },
    }));
    closeButton->setPosition(closeX, headerY);
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
  if (!isOpen()) {
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
