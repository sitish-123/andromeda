#include "ui/files/file_gallery.h"

#include "cursor-shape-v1-client-protocol.h"

#include "core/log.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <format>
#include <wayland-client-protocol.h>
#include <linux/input-event-codes.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("file-gallery");
  // The Gallery follows the scheme: window background stage, popover glass capsules with a stroke, label text.
  // Mockup .gcap 56 %, .ginfo 64 %: the wash under them is already blurred, so a see-through fill reads as glass.
  constexpr float kCapsuleAlpha = 0.56F;
  constexpr float kCardAlpha = 0.64F;
  ColorSpec role(ColorRole r) { return colorSpecFromRole(r); }
  constexpr float kAmbientOpacity = 0.56F;
  constexpr int kAmbientPx = 64; // DESIGN.md: the cached 64 px thumbnail, scaled up
  constexpr int kFilmPx = 96;
  constexpr int kPhotoMaxPx = 2048;
  constexpr std::size_t kFilmSide = 7; // neighbours shown on each side of the current photo
  constexpr auto kChromeIdle = std::chrono::seconds(2);

  const float& kCapsuleH = Tokens::spaceP40;
  const float& kFilmH = Tokens::spaceP64;
  const float& kFilmItemW = Tokens::spaceP32;
  const float& kFilmItemH = Tokens::spaceP48;
  const float& kEdge = Tokens::spaceP16;
  constexpr float kInfoW = 280.0F; // mockup .ginfo width, 4-grid
  constexpr float kMapH = 112.0F;   // mockup .gmap
  constexpr float kZoomStep = 1.25F; // per wheel step
  constexpr float kZoomMax = 8.0F;   // past 100 %: the photo's pixels get large
  constexpr int kZoomPx = 8192;      // decode size while zoomed: full resolution for most photos
  constexpr auto kDoubleClick = std::chrono::milliseconds(400);
  constexpr float kNavW = 96.0F;     // mockup .gnavi
  constexpr float kNavH = 72.0F;


  // A capsule with an input area over all of it.
  class Capsule final : public Flex {
  public:
    InputArea* area = nullptr;

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      placeArea();
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      placeArea();
    }

  private:
    void placeArea() {
      if (area != nullptr) {
        area->setPosition(0.0F, 0.0F);
        area->setSize(width(), height());
      }
    }
  };

  // Removes a flex's content; its fill node is a child too, outside the layout, and stays.
  void clearContent(Flex& flex) {
    std::vector<Node*> content;
    for (const auto& child : flex.children()) {
      if (child->participatesInLayout()) {
        content.push_back(child.get());
      }
    }
    for (Node* child : content) {
      (void)flex.removeChild(child);
    }
  }

  std::unique_ptr<Capsule> capsule(float s, float radius) {
    auto c = std::make_unique<Capsule>();
    c->setDirection(FlexDirection::Horizontal);
    c->setAlign(FlexAlign::Center);
    c->setFill(colorSpecFromRole(ColorRole::Popover, kCapsuleAlpha));
    c->setBorder(role(ColorRole::Stroke), std::max(1.0F, s / 2.0F));
    c->setRadius(radius);
    return c;
  }

  std::unique_ptr<Capsule> arrow(const char* glyph, float s, std::function<void()> onClick) {
    const float side = kCapsuleH * s;
    auto c = capsule(s, side / 2.0F);
    c->setJustify(FlexJustify::Center);
    c->setMinWidth(side);
    c->setMaxWidth(side);
    c->setMinHeight(side);
    c->setMaxHeight(side);
    c->addChild(ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s, .color = role(ColorRole::Label1)}));
    auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .participatesInLayout = false});
    area->setOnClick([onClick = std::move(onClick)](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        onClick();
      }
    });
    c->area = static_cast<InputArea*>(c->addChild(std::move(area)));
    return c;
  }
  // A 32 round button of the actions capsule; the "on" state fills it (mockup .gcap .gb.on).
  std::unique_ptr<Capsule> actionButton(const char* glyph, float s, Glyph** out,
                                        std::function<void(float sceneX, float sceneY)> onClick) {
    const float side = Tokens::spaceP32 * s;
    auto c = std::make_unique<Capsule>();
    c->setDirection(FlexDirection::Horizontal);
    c->setAlign(FlexAlign::Center);
    c->setJustify(FlexJustify::Center);
    c->setRadius(side / 2.0F);
    c->setMinWidth(side);
    c->setMaxWidth(side);
    c->setMinHeight(side);
    c->setMaxHeight(side);
    c->addChild(ui::glyph({.out = out, .glyph = std::string(glyph), .glyphSize = Tokens::sizeSymbol * s,
                           .color = role(ColorRole::Label1)}));
    auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .participatesInLayout = false});
    area->setOnClick([onClick = std::move(onClick)](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        onClick(data.sceneX, data.sceneY);
      }
    });
    c->area = static_cast<InputArea*>(c->addChild(std::move(area)));
    return c;
  }

  // Fixed decode sizes: a live resize must not start a decode (and a disk cache file) per frame.
  int photoBucket(float px) {
    for (const int bucket : {512, 1024, kPhotoMaxPx}) {
      if (px <= static_cast<float>(bucket)) {
        return bucket;
      }
    }
    return kPhotoMaxPx;
  }
} // namespace

FileGallery::FileGallery(float scale, ThumbnailService* thumbnails, AnimationManager& animations, Host host,
                         std::shared_ptr<State> state)
    : m_scale(scale), m_thumbs(thumbnails), m_host(std::move(host)), m_animations(animations), m_state(std::move(state)) {
  const float s = scale;
  m_bg = static_cast<Box*>(addChild(ui::box({.fill = role(ColorRole::SolidRegular)})));
  m_ambientOld = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_ambientOld->setFit(ImageFit::Cover);
  m_ambientOld->setVisible(false);
  m_ambient = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_ambient->setFit(ImageFit::Cover);
  m_ambient->setOpacity(kAmbientOpacity);
  m_photoShadow = static_cast<Box*>(addChild(ui::box(
      {.fill = role(ColorRole::Shadow), .radius = Style::scaledRadius(Tokens::radiusR8, s), .visible = false})));
  m_photoShadow->setFill(colorSpecFromRole(ColorRole::Shadow, isResolvedLightTheme() ? 0.24F : 0.5F));
  m_photoShadow->setSoftness(Tokens::spaceP64 * s);
  m_photo = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_photo->setFit(ImageFit::Contain);
  m_photo->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
  // Under the capsules: a move anywhere brings the chrome back and restarts its idle time.
  auto hover = ui::inputArea({.participatesInLayout = false});
  hover->setOnEnter([this](const InputArea::PointerData&) { showChrome(); });
  hover->setOnMotion([this](const InputArea::PointerData& data) {
    showChrome();
    if (!m_dragging || !m_hover->pressed()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float, std::milli>(now - m_dragAt).count();
    if (dt > 0.0F && dt < 80.0F) {
      m_velX = (data.localX - m_dragX) / dt;
      m_velY = (data.localY - m_dragY) / dt;
    }
    m_panX += data.localX - m_dragX;
    m_panY += data.localY - m_dragY;
    m_dragX = data.localX;
    m_dragY = data.localY;
    m_dragAt = now;
    clampPan();
    refresh();
  });
  // Left presses zoom and pan the photo; a right press goes on to the stage (menus). An unused scroll goes on too.
  hover->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  hover->setAcceptedScrollDirections(InputArea::allScrollDirections());
  hover->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      const auto now = std::chrono::steady_clock::now();
      // Double-click: fit <-> 100 % at the pointer (mockup section 15).
      if (now - m_lastClickAt < kDoubleClick) {
        m_lastClickAt = {};
        m_wantHundred = m_zoom <= 1.01F;
        m_hundredSteps = 1.0F;
        zoomAt(data.localX, data.localY, m_zoom > 1.01F ? 1.0F : std::max(zoom100(), kZoomStep));
        return;
      }
      m_lastClickAt = now;
      m_animations.cancel(m_panAnimX);
      m_animations.cancel(m_panAnimY);
      m_dragging = m_zoom > 1.0F;
      m_dragX = data.localX;
      m_dragY = data.localY;
      m_velX = m_velY = 0.0F;
      m_dragAt = now;
      return;
    }
    if (m_dragging) {
      m_dragging = false;
      kLog.debug("gallery pan {} {}", std::lround(m_panX), std::lround(m_panY)); // tests/e2e/files_gallery.sh
      const float idleMs =
          std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_dragAt).count();
      flingPan(idleMs > 80.0F ? 0.0F : m_velX, idleMs > 80.0F ? 0.0F : m_velY);
    }
  });
  hover->setOnAxisHandler([this](const InputArea::PointerData& data) {
    const bool finger = data.axisSource == WL_POINTER_AXIS_SOURCE_FINGER;
    if (finger) {
      // A two-finger scroll pans a zoomed photo; at fit it is not ours.
      if (m_zoom <= 1.0F) {
        return false;
      }
      (data.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? m_panY : m_panX) -= static_cast<float>(data.axisValue);
      clampPan();
      refresh();
      return true;
    }
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    const float steps = data.axisLines != 0.0F ? data.axisLines : static_cast<float>(data.axisValue) / 10.0F;
    const float factor = std::pow(kZoomStep, -steps);
    if (m_wantHundred) {
      m_hundredSteps *= factor; // applied again on the true 100 % once the full decode tells the size
    }
    zoomAt(data.localX, data.localY, m_zoom * factor);
    return true;
  });
  hover->setOnPinch([this](const InputArea::PinchData& data) {
    if (data.phase == InputArea::PinchData::Phase::Begin) {
      m_pinchStartZoom = m_zoom;
      return;
    }
    if (data.phase == InputArea::PinchData::Phase::End) {
      if (m_zoom < 1.0F) {
        zoomAt(m_stageCx, m_stageCy, 1.0F); // a pinch below fit springs back
      }
      return;
    }
    // Below fit only a little (rubber band), so the spring back reads as one.
    const float target = m_pinchStartZoom * static_cast<float>(data.scale);
    zoomAt(data.localX, data.localY, target < 1.0F ? 1.0F - (1.0F - target) * 0.3F : target);
  });
  m_hover = static_cast<InputArea*>(addChild(std::move(hover)));

  // Mockup .gcap: padding 0 4, a 32 back button, then place or name over "12 of 86 · date" (.gt: padding 0 8).
  auto title = capsule(s, kCapsuleH * s / 2.0F);
  title->setPadding(0.0F, Tokens::spaceP12 * s, 0.0F, Tokens::spaceP4 * s);
  title->setMinHeight(kCapsuleH * s);
  title->setMaxHeight(kCapsuleH * s);
  title->addChild(actionButton("chevron-left", s, nullptr, [this](float, float) {
    if (m_host.back) {
      m_host.back();
    }
  }));
  auto text = std::make_unique<Flex>();
  text->setDirection(FlexDirection::Vertical);
  text->setJustify(FlexJustify::Center);
  text->setPadding(0.0F, Tokens::spaceP8 * s);
  text->addChild(ui::label({.out = &m_name, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                             .color = role(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
  text->addChild(ui::label({.out = &m_position, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                            .color = role(ColorRole::Label2), .maxLines = 1}));
  title->addChild(std::move(text));
  m_title = static_cast<Flex*>(addChild(std::move(title)));

  const auto step = [this](int delta) {
    if (m_items.empty()) {
      return;
    }
    const auto next = static_cast<std::ptrdiff_t>(m_current) + delta;
    if (next >= 0 && next < static_cast<std::ptrdiff_t>(m_items.size()) && m_host.select) {
      m_host.select(static_cast<std::size_t>(next));
    }
  };
  m_prev = static_cast<Flex*>(addChild(arrow("chevron-left", s, [step] { step(-1); })));
  m_next = static_cast<Flex*>(addChild(arrow("chevron-right", s, [step] { step(1); })));

  auto actions = capsule(s, kCapsuleH * s / 2.0F);
  actions->setGap(Tokens::spaceP2 * s);
  actions->setPadding(0.0F, Tokens::spaceP4 * s);
  actions->setMinHeight(kCapsuleH * s);
  actions->setMaxHeight(kCapsuleH * s);
  const auto act = [this](const char* id) {
    return [this, id](float x, float y) {
      kLog.debug("gallery action {}", id); // tests/e2e/files_gallery.sh
      if (m_host.action) {
        m_host.action(id, x, y);
      }
    };
  };
  const auto add = [&](const char* id, std::unique_ptr<Capsule> button) {
    Node* node = actions->addChild(std::move(button));
    m_actionIds.emplace_back(node, id);
    return static_cast<Flex*>(node);
  };
  add("favourite", actionButton("star", s, &m_starGlyph, act("favourite")));
  m_rotateButton = add("rotate", actionButton("rotate-clockwise", s, nullptr, act("rotate")));
  add("markup", actionButton("pencil", s, nullptr, act("markup")));
  m_phoneButton = add("phone", actionButton("device-mobile", s, nullptr, act("phone")));
  add("share", actionButton("share", s, nullptr, act("share")));
  m_infoButton = add("info", actionButton("info-circle", s, nullptr, [this](float, float) { toggleInfo(); }));
  add("more", actionButton("dots", s, nullptr, act("more")));
  m_actions = static_cast<Flex*>(addChild(std::move(actions)));

  // The Info card (mockup .ginfo): 280 wide, 20 radius, 16 padding, 12 gap; filled by buildInfoCard.
  auto card = capsule(s, Style::scaledRadius(Tokens::componentPopoverRadius, s));
  card->setFill(colorSpecFromRole(ColorRole::Popover, kCardAlpha));
  card->setDirection(FlexDirection::Vertical);
  card->setAlign(FlexAlign::Stretch);
  card->setGap(Tokens::spaceP12 * s);
  card->setPadding(Tokens::spaceP16 * s, Tokens::spaceP16 * s);
  card->setMinWidth(kInfoW * s);
  card->setMaxWidth(kInfoW * s);
  card->setVisible(false);
  m_infoCard = static_cast<Flex*>(addChild(std::move(card)));

  auto film = capsule(s, Style::scaledRadius(Tokens::radiusR16, s));
  film->setGap(Tokens::spaceP4 * s);
  film->setPadding(Tokens::spaceP8 * s, Tokens::spaceP8 * s);
  film->setMinHeight(kFilmH * s);
  film->setMaxHeight(kFilmH * s);
  m_film = static_cast<Flex*>(addChild(std::move(film)));
  m_filmRing = static_cast<Box*>(addChild(ui::box({.radius = Style::scaledRadius(Tokens::radiusR8, s)})));
  m_filmRing->setBorder(role(ColorRole::Label1), Tokens::spaceP2 * s);
  m_filmRing->setZIndex(2);
  m_filmRing->setHitTestVisible(false);

  // While zoomed: the navigator bottom right (the whole photo, a frame for the view) and the zoom chip bottom left.
  m_navigator = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_navigator->setFit(ImageFit::Cover);
  m_navigator->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
  m_navigator->setBorder(role(ColorRole::Stroke), std::max(1.0F, s / 2.0F));
  m_navigator->setHitTestVisible(false);
  m_navigatorFrame = static_cast<Box*>(addChild(ui::box({.radius = Tokens::spaceP2 * s})));
  m_navigatorFrame->setBorder(role(ColorRole::Label1), Tokens::spaceP2 * s);
  m_navigatorFrame->setHitTestVisible(false);
  auto chip = capsule(s, Tokens::spaceP32 * s / 2.0F);
  chip->setPadding(0.0F, Tokens::spaceP12 * s);
  chip->setMinHeight(Tokens::spaceP32 * s);
  chip->setMaxHeight(Tokens::spaceP32 * s);
  chip->setHitTestVisible(false);
  chip->addChild(ui::label({.out = &m_zoomLabel, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::SemiBold,
                            .color = role(ColorRole::Label1), .maxLines = 1}));
  m_zoomChip = static_cast<Flex*>(addChild(std::move(chip)));

  // Scrubber (mockup .gscrub): play/pause, a progress capsule, "0:17 / 0:42". Progress only: the helper cannot seek.
  auto scrub = capsule(s, kCapsuleH * s / 2.0F);
  scrub->setGap(Tokens::spaceP12 * s);
  scrub->setPadding(0.0F, Tokens::spaceP16 * s);
  scrub->setMinHeight(kCapsuleH * s);
  scrub->setMaxHeight(kCapsuleH * s);
  auto play = actionButton("player-play", s, &m_scrubGlyph, [this](float, float) { togglePlay(); });
  scrub->addChild(std::move(play));
  auto track = ui::flex(FlexDirection::Horizontal, {.fill = role(ColorRole::Fill2), .radius = Tokens::spaceP2 * s,
                                                    .minHeight = Tokens::spaceP4 * s, .maxHeight = Tokens::spaceP4 * s,
                                                    .flexGrow = 1.0F});
  auto level = ui::box({.fill = role(ColorRole::Label1), .radius = Tokens::spaceP2 * s});
  level->setParticipatesInLayout(false);
  m_scrubLevel = static_cast<Box*>(track->addChild(std::move(level)));
  m_scrubTrack = static_cast<Flex*>(scrub->addChild(std::move(track)));
  scrub->addChild(ui::label({.out = &m_scrubTime, .text = std::string("0:00"), .fontSize = Style::fontSizeCaption * s,
                             .fontWeight = FontWeight::Medium, .color = role(ColorRole::Label1), .maxLines = 1}));
  scrub->setVisible(false);
  m_scrub = static_cast<Flex*>(addChild(std::move(scrub)));
  setClipChildren(true); // a zoomed photo is larger than the stage
  if (m_state != nullptr && !m_state->item.empty()) {
    m_zoom = m_state->zoom;
    m_panX = m_state->panX;
    m_panY = m_state->panY;
    m_infoOpen = m_state->infoOpen;
    m_infoFor = m_state->infoFor;
    m_info = std::move(m_state->info);
    m_mapDirty = !m_info.mapBgra.empty();
    if (!m_info.name.empty()) {
      buildInfoCard();
    }
    m_videoActive = m_state->videoActive;
    m_scrubPlaying = m_state->scrubPlaying;
    m_scrubProgress = m_state->scrubProgress;
    m_scrubTime->setText(m_state->scrubTime);
    m_washPath = m_state->washPath;
    if (m_state->slideshowOn) {
      m_slideshowOn = false;
      togglePlay(); // restarts the slideshow timer on this node
    }
  }
}

float FileGallery::zoom100() const {
  return m_fitW > 0.0F && m_photoTexW > 0 ? static_cast<float>(m_photoTexW) / m_fitW : 1.0F;
}

void FileGallery::zoomAt(float x, float y, float zoom) {
  if (currentIsVideo() || m_videoActive) {
    return; // a playing video sits on its own subsurface at fit
  }
  zoom = std::clamp(zoom, 0.5F, std::max(kZoomMax, zoom100()));
  if (m_fitW <= 0.0F || zoom == m_zoom) {
    return;
  }
  // The photo point under (x, y) stays there: its offset from the photo centre scales with the zoom.
  const float k = zoom / m_zoom;
  const float ox = x - (m_stageCx + m_panX);
  const float oy = y - (m_stageCy + m_panY);
  m_panX -= ox * (k - 1.0F);
  m_panY -= oy * (k - 1.0F);
  const bool crossed = (m_zoom > 1.0F) != (zoom > 1.0F);
  m_zoom = zoom;
  clampPan();
  if (m_zoom <= 1.0F) {
    kLog.debug("gallery zoom fit"); // tests/e2e/files_gallery.sh
  } else {
    kLog.debug("gallery zoom {}", std::lround(m_zoom * 100.0F / zoom100()));
  }
  if (crossed) {
    showChrome(); // the filmstrip leaves for the navigator, or comes back
  }
  refresh();
}

void FileGallery::resetZoom() {
  m_animations.cancel(m_panAnimX);
  m_animations.cancel(m_panAnimY);
  m_zoom = 1.0F;
  m_wantHundred = false;
  m_panX = m_panY = 0.0F;
  m_dragging = false;
}

void FileGallery::clampPan() {
  // The photo may not leave a gap at an edge it overflows; a photo smaller than the stage stays centred.
  const float overX = std::max(0.0F, (m_fitW * m_zoom - m_stageCx * 2.0F) / 2.0F);
  const float overY = std::max(0.0F, (m_fitH * m_zoom - m_stageCy * 2.0F) / 2.0F);
  m_panX = std::clamp(m_panX, -overX, overX);
  m_panY = std::clamp(m_panY, -overY, overY);
}

void FileGallery::flingPan(float vx, float vy) {
  // Inertia: the pan glides on in the release direction for about 300 ms of travel, then stops at the edge.
  const auto glide = [this](AnimationManager::Id& id, float& pan, float v, float limit) {
    if (std::abs(v) < 0.05F) {
      return;
    }
    const float target = std::clamp(pan + v * 300.0F, -limit, limit);
    id = m_animations.drive(id, pan, target, Style::spring(Tokens::springFollow), [this, &pan](float value) {
      pan = value;
      refresh();
    }, {}, this, v * 1000.0F, Unit::Px);
  };
  glide(m_panAnimX, m_panX, vx, std::max(0.0F, (m_fitW * m_zoom - m_stageCx * 2.0F) / 2.0F));
  glide(m_panAnimY, m_panY, vy, std::max(0.0F, (m_fitH * m_zoom - m_stageCy * 2.0F) / 2.0F));
}

FileGallery::~FileGallery() {
  *m_alive = false;
  m_slideshow.stop();
  // A rebuild replaces this node: the next one takes over (the video keeps playing on the host's subsurface).
  if (m_state != nullptr) {
    *m_state = State{.item = m_items.empty() ? fs::path() : m_items[m_current],
                     .zoom = m_zoom,
                     .panX = m_panX,
                     .panY = m_panY,
                     .infoOpen = m_infoOpen,
                     .infoFor = m_infoFor,
                     .info = std::move(m_info),
                     .videoActive = m_videoActive,
                     .scrubPlaying = m_scrubPlaying,
                     .scrubProgress = m_scrubProgress,
                     .scrubTime = std::string(m_scrubTime->text()),
                     .slideshowOn = m_slideshowOn,
                     .washPath = m_washPath};
  }
  m_animations.cancel(m_chromeAnim);
  m_animations.cancel(m_washAnim);
  m_animations.cancel(m_panAnimX);
  m_animations.cancel(m_panAnimY);
  releaseAll(m_held);
}

void FileGallery::showChrome() {
  driveChrome(1.0F);
  m_hideTimer.start(kChromeIdle, [this] { driveChrome(0.0F); });
}

void FileGallery::toggleChrome() {
  if (m_chromeTarget > 0.5F) {
    m_hideTimer.stop();
    driveChrome(0.0F);
  } else {
    showChrome();
  }
}

void FileGallery::toggleInfo() {
  m_infoOpen = !m_infoOpen;
  kLog.debug("gallery info {}", m_infoOpen ? "shown" : "hidden"); // tests/e2e/files_gallery.sh
  showChrome();
  requestInfo();
  refresh();
}

void FileGallery::requestInfo() {
  if (!m_infoOpen || m_items.empty() || !m_host.info || m_infoFor == m_items[m_current]) {
    return;
  }
  m_infoFor = m_items[m_current];
  const float s = m_scale;
  const int mapW = static_cast<int>(std::lround((kInfoW - Tokens::spaceP16 * 2.0F) * s));
  const int mapH = static_cast<int>(std::lround(kMapH * s));
  m_host.info(m_infoFor, mapW, mapH, [this, alive = std::weak_ptr<bool>(m_alive), path = m_infoFor](PhotoInfo info) {
    if (alive.expired() || path != m_infoFor) {
      return; // closed, or the user moved on to another photo
    }
    kLog.debug("gallery info for {} map {}", path.filename().string(), info.mapBgra.empty() ? "none" : info.place);
    m_info = std::move(info);
    m_mapDirty = true;
    buildInfoCard();
    refresh();
  });
}

void FileGallery::buildInfoCard() {
  clearContent(*m_infoCard);
  m_map = nullptr;
  const float s = m_scale;
  auto head = ui::flex(FlexDirection::Vertical, {.align = FlexAlign::Start});
  head->addChild(ui::label({.text = m_info.name, .fontSize = Style::fontSizeTitle * s, .fontWeight = FontWeight::SemiBold,
                            .color = role(ColorRole::Label1), .maxLines = 2, .ellipsize = TextEllipsize::Middle}));
  head->addChild(ui::label({.text = m_info.kind, .fontSize = Style::fontSizeCaption * s, .color = role(ColorRole::Label2),
                            .maxLines = 1}));
  m_infoCard->addChild(std::move(head));
  if (!m_info.chips.empty()) {
    // A set width: the card's measure gives a stretched row no width, so it would count the chips as one line.
    auto chips = ui::flex(FlexDirection::Horizontal,
                          {.wrap = true, .gap = Tokens::spaceP4 * s, .width = (kInfoW - 2.0F * Tokens::spaceP16) * s});
    for (const std::string& chip : m_info.chips) {
      auto c = ui::flex(FlexDirection::Horizontal,
                        {.align = FlexAlign::Center, .paddingV = 0.0F, .paddingH = Tokens::spaceP8 * s,
                         .fill = role(ColorRole::Fill1), .radius = Tokens::spaceP24 * s / 2.0F,
                         .minHeight = Tokens::spaceP24 * s});
      c->addChild(ui::label({.text = chip, .fontSize = Style::fontSizeCaption * s, .color = role(ColorRole::Label1),
                             .maxLines = 1}));
      chips->addChild(std::move(c));
    }
    m_infoCard->addChild(std::move(chips));
  }
  if (!m_info.mapBgra.empty()) {
    // A fixed 112 slot; the map image fills it in doLayout (an Image has no size of its own until uploaded).
    auto slot = ui::flex(FlexDirection::Horizontal, {.minHeight = kMapH * s, .maxHeight = kMapH * s});
    auto map = std::make_unique<Image>();
    map->setRadius(Style::scaledRadius(Tokens::radiusR12, s));
    map->setFit(ImageFit::Cover);
    map->setParticipatesInLayout(false);
    m_map = static_cast<Image*>(slot->addChild(std::move(map)));
    m_infoCard->addChild(std::move(slot));
    m_infoCard->addChild(ui::label({.text = m_info.place, .fontSize = Style::fontSizeCaption * s,
                                    .fontWeight = FontWeight::SemiBold, .color = role(ColorRole::Label1), .maxLines = 1}));
  }
  for (const auto& [key, value] : m_info.rows) {
    auto row = ui::flex(FlexDirection::Horizontal, {.justify = FlexJustify::SpaceBetween, .gap = Tokens::spaceP12 * s});
    row->addChild(ui::label({.text = key, .fontSize = Style::fontSizeCaption * s, .color = role(ColorRole::Label2),
                             .maxLines = 1}));
    row->addChild(ui::label({.text = value, .fontSize = Style::fontSizeCaption * s, .color = role(ColorRole::Label1),
                             .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
    m_infoCard->addChild(std::move(row));
  }
}

void FileGallery::enter(std::shared_ptr<State> state) {
  m_state = std::move(state);
  resetZoom();
  m_infoOpen = false;
  m_videoActive = false;
  m_videoRect = {};
  m_slideshowOn = false;
  m_slideshow.stop();
  m_items.clear(); // the next sync counts as a new photo: its motion plays, its timings log
  m_itemInfo.clear();
  refresh();
}

void FileGallery::stopVideo() {
  ++m_motionGeneration;
  if (m_videoActive) {
    m_videoActive = false;
    m_videoRect = {};
    if (m_host.stopVideo) {
      m_host.stopVideo();
    }
  }
}

void FileGallery::startMotion() {
  if (m_items.empty() || !m_host.playVideo) {
    return;
  }
  const ItemInfo& item = m_itemInfo[m_current];
  const fs::path photo = m_items[m_current];
  if (!item.motionVideo.empty()) {
    m_videoActive = true;
    m_videoRect = {}; // placed by the next layout
    m_host.playVideo(item.motionVideo, false, m_videoRect);
    refresh();
    return;
  }
  if (!m_host.motionVideo) {
    return;
  }
  const auto generation = ++m_motionGeneration;
  m_host.motionVideo(photo, [this, alive = std::weak_ptr<bool>(m_alive), generation, photo](fs::path video) {
    if (alive.expired() || generation != m_motionGeneration || video.empty() || m_items.empty() ||
        m_items[m_current] != photo) {
      return;
    }
    m_videoActive = true;
    m_videoRect = {};
    m_host.playVideo(video, false, m_videoRect);
    refresh();
  });
}

void FileGallery::togglePlay() {
  if (m_items.empty()) {
    return;
  }
  if (currentIsVideo()) {
    if (!m_videoActive) {
      if (m_host.playVideo) {
        m_videoActive = true;
        m_videoRect = {};
        m_host.playVideo(m_items[m_current], true, m_videoRect);
      }
    } else if (m_host.toggleVideo) {
      m_host.toggleVideo();
    }
    refresh();
    return;
  }
  // Photos: a slideshow, one photo every 3 s, until Space again or the last photo.
  m_slideshowOn = !m_slideshowOn;
  kLog.debug("gallery slideshow {}", m_slideshowOn ? "on" : "off"); // tests/e2e/files_gallery.sh
  if (!m_slideshowOn) {
    m_slideshow.stop();
    return;
  }
  m_slideshow.startRepeating(std::chrono::seconds(3), [this] {
    if (m_current + 1 >= m_items.size() || !m_host.select) {
      m_slideshowOn = false;
      m_slideshow.stop();
      return;
    }
    m_host.select(m_current + 1);
  });
}

void FileGallery::setVideoProgress(bool playing, float progress, const std::string& time) {
  m_scrubPlaying = playing;
  m_scrubProgress = progress;
  m_scrubTime->setText(time);
  refresh();
}

void FileGallery::videoEnded() {
  m_videoActive = false;
  m_videoRect = {};
  m_scrubPlaying = false;
  refresh();
}

void FileGallery::appendGeometry(std::string& out) const {
  if (!visible() || !m_actions->visible()) {
    return;
  }
  for (const auto& [node, id] : m_actionIds) {
    if (node->visible()) {
      float x = 0.0F;
      float y = 0.0F;
      Node::mapToScene(node, node->width() / 2.0F, node->height() / 2.0F, x, y);
      out += std::format("gallery-action {} {} {}\n", id, static_cast<int>(x), static_cast<int>(y));
    }
  }
}

void FileGallery::refresh() {
  markLayoutDirty();
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileGallery::driveChrome(float target) {
  if (target == m_chromeTarget) {
    return;
  }
  m_chromeTarget = target;
  kLog.debug("gallery chrome {}", target > 0.5F ? "shown" : "hidden"); // tests/e2e/files_gallery.sh
  // spring.follow: the photo grows into the space the capsules free (DESIGN.md "Files v2 immersive Gallery").
  m_chromeAnim = m_animations.drive(m_chromeAnim, m_chrome, target, Style::spring(Tokens::springFollow), [this](float v) {
    m_chrome = v;
    markLayoutDirty();
    if (m_host.invalidate) {
      m_host.invalidate();
    }
  }, {}, this);
  // The spring ticks on frames; from the idle timer nothing else asks for one.
  markLayoutDirty();
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileGallery::setItems(std::vector<fs::path> items, std::size_t current, std::vector<ItemInfo> info) {
  // A fresh node after a rebuild: the state's photo counts as shown, so its zoom, card and video carry over.
  const fs::path before = !m_items.empty() ? m_items[m_current] : m_state != nullptr ? m_state->item : fs::path();
  m_items = std::move(items);
  m_itemInfo = std::move(info);
  m_itemInfo.resize(m_items.size());
  m_current = m_items.empty() ? 0 : std::min(current, m_items.size() - 1);
  if (m_items.empty() || m_items[m_current] != before) {
    resetZoom(); // another photo opens at fit
    m_shownAt = std::chrono::steady_clock::now();
    m_loggedThumb = m_loggedFull = false;
    stopVideo();
    m_scrubProgress = 0.0F;
    m_scrubPlaying = false;
    m_scrubTime->setText("0:00");
    if (!m_items.empty() && m_itemInfo[m_current].motion) {
      startMotion(); // a motion photo plays once on open, muted (mockup section 15)
    }
  }
  m_filmDirty = true; // the filmstrip window moves with the current photo
  showChrome();
  requestInfo();
  markLayoutDirty();
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileGallery::hold(const std::string& path, int px, std::vector<Held>& next, bool now) {
  if (m_thumbs == nullptr) {
    return;
  }
  (void)m_thumbs->acquire(path, px, now ? ThumbnailService::Priority::Now : ThumbnailService::Priority::Visible);
  next.push_back({.path = path, .px = px});
}

void FileGallery::releaseAll(std::vector<Held>& held) {
  if (m_thumbs != nullptr) {
    for (const Held& h : held) {
      m_thumbs->release(h.path, h.px);
    }
  }
  held.clear();
}

void FileGallery::rebuildFilm() {
  clearContent(*m_film);
  m_filmImages.clear();
  m_filmIndex.clear();
  const float s = m_scale;
  const std::size_t first = m_current > kFilmSide ? m_current - kFilmSide : 0;
  const std::size_t last = std::min(m_items.size(), m_current + kFilmSide + 1);
  for (std::size_t i = first; i < last; ++i) {
    if (i > first && m_itemInfo[i].day != m_itemInfo[i - 1].day) {
      // A new day (mockup .film b): a 1 x 32 hairline with 4 px on each side.
      auto divider = ui::box({.fill = role(ColorRole::Separator), .width = std::max(1.0F, s), .height = Tokens::spaceP32 * s});
      m_film->addChild(std::move(divider));
    }
    auto cell = std::make_unique<Capsule>();
    const float w = (i == m_current ? m_filmCurrentW : kFilmItemW) * s; // the current one settles at its aspect in layout
    cell->setMinWidth(w);
    cell->setMaxWidth(w);
    cell->setMinHeight(kFilmItemH * s);
    cell->setMaxHeight(kFilmItemH * s);
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Cover);
    image->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
    image->setSize(w, kFilmItemH * s);
    image->setOpacity(i == m_current ? 1.0F : 0.72F);
    m_filmImages.push_back(static_cast<Image*>(cell->addChild(std::move(image))));
    m_filmIndex.push_back(i);
    if (m_itemInfo[i].motion) {
      // Mockup .film .vm: a 6 px white dot at the bottom left marks a motion photo.
      const float dot = 6.0F * s;
      auto mark = ui::box({.radius = dot / 2.0F});
      mark->setFill(Color{1.0F, 1.0F, 1.0F, 1.0F}); // white on every scheme, as the mockup's ring
      mark->setParticipatesInLayout(false);
      mark->setPosition(Tokens::spaceP2 * s, kFilmItemH * s - Tokens::spaceP2 * s - dot);
      mark->setSize(dot, dot);
      mark->setZIndex(1);
      cell->addChild(std::move(mark));
    }
    auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .participatesInLayout = false});
    area->setOnClick([this, i](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT && m_host.select) {
        m_host.select(i);
      }
    });
    if (i == m_current && m_itemInfo[i].motion) {
      // Hovering the current motion photo's cell plays its motion again (mockup: "on hover over the dot").
      area->setOnEnter([this](const InputArea::PointerData&) {
        if (!m_videoActive) {
          startMotion();
        }
      });
    }
    cell->area = static_cast<InputArea*>(cell->addChild(std::move(area)));
    m_film->addChild(std::move(cell));
  }
  m_filmDirty = false;
}

void FileGallery::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setSize(rect.width, rect.height);
  doLayout(renderer);
}

void FileGallery::doLayout(Renderer& renderer) {
  const float s = m_scale;
  const float w = width();
  const float h = height();
  m_bg->setSize(w, h);
  m_hover->setPosition(0.0F, 0.0F);
  m_hover->setSize(w, h);
  if (m_filmDirty) {
    rebuildFilm();
  }
  const bool any = !m_items.empty();
  // Hidden chrome takes no input: a click on the photo where a capsule was must not pick a neighbour.
  const bool chrome = m_chrome > 0.01F;
  // Zoomed, the navigator and the zoom chip take the filmstrip's and the arrows' place (mockup section 15 right).
  const bool zoomed = m_zoom > 1.0F;
  for (Node* n : {static_cast<Node*>(m_title), static_cast<Node*>(m_actions), static_cast<Node*>(m_film),
                  static_cast<Node*>(m_filmRing), static_cast<Node*>(m_prev), static_cast<Node*>(m_next),
                  static_cast<Node*>(m_navigator), static_cast<Node*>(m_navigatorFrame), static_cast<Node*>(m_zoomChip)}) {
    n->setOpacity(m_chrome);
  }
  for (Node* n : {static_cast<Node*>(m_title), static_cast<Node*>(m_actions)}) {
    n->setVisible(any && chrome);
  }
  // A video shows its scrubber in the filmstrip's place (mockup section 15 middle).
  const bool video = any && currentIsVideo();
  m_film->setVisible(any && chrome && !zoomed && !video);
  m_filmRing->setVisible(any && chrome && !zoomed && !video);
  m_scrub->setVisible(video && chrome);
  m_scrub->setOpacity(m_chrome);
  m_prev->setVisible(any && chrome && !zoomed && m_current > 0);
  m_next->setVisible(any && chrome && !zoomed && m_current + 1 < m_items.size());
  m_navigator->setVisible(false);
  m_navigatorFrame->setVisible(false);
  m_zoomChip->setVisible(false);
  if (!any) {
    m_photo->setVisible(false);
    m_photoShadow->setVisible(false);
    m_ambient->setVisible(false);
    releaseAll(m_held);
    return;
  }

  // Textures: the photo at the stage size, its 64 px wash, the filmstrip crops; the old holds go after the new ones.
  std::vector<Held> next;
  const std::string current = m_items[m_current].string();
  const int photoPx = photoBucket(std::ceil(std::max(w, h) * s));
  hold(current, photoPx, next, true);
  if (zoomed) {
    hold(current, kZoomPx, next, true); // full resolution while zoomed; the stage-size one shows until it is ready
  }
  hold(current, kAmbientPx, next);
  hold(current, kFilmPx, next, true); // the stand-in while the stage size decodes
  if (!m_oldWashPath.empty()) {
    hold(m_oldWashPath, kAmbientPx, next); // until the cross-fade ends
  }
  for (std::size_t k = 0; k < m_filmIndex.size(); ++k) {
    hold(m_items[m_filmIndex[k]].string(), kFilmPx, next);
  }
  releaseAll(m_held);
  m_held = std::move(next);

  const auto texture = [this](const std::string& path, int px) {
    return m_thumbs != nullptr ? m_thumbs->peek(path, px) : TextureHandle{};
  };
  TextureHandle photo = texture(current, photoPx);
  if (const TextureHandle full = zoomed ? texture(current, kZoomPx) : TextureHandle{}; full.id != 0) {
    photo = full;
    m_photoTexW = full.width;
    if (m_wantHundred) {
      // The full decode gave the real size: settle the double-click on true 100 %, the view centre kept.
      m_wantHundred = false;
      zoomAt(m_stageCx, m_stageCy, std::max(zoom100(), kZoomStep) * m_hundredSteps);
      m_hundredSteps = 1.0F;
    }
  } else if (photo.id != 0 && m_photoTexW < photo.width) {
    m_photoTexW = photo.width; // until the full decode tells the real size
  }
  // The filmstrip already shows this photo's 96 px crop: it stands in for the wash and the photo until their own
  // decodes land, so a large photo never opens on an empty stage.
  const TextureHandle film = texture(current, kFilmPx);
  TextureHandle wash = texture(current, kAmbientPx);
  if (wash.id == 0) {
    wash = film;
  }
  if (photo.id == 0) {
    photo = wash; // the cached small one at once, the full size swaps in (DESIGN.md speed)
  }

  const float inset = Tokens::spaceP64 * s;
  const auto mix = [this](float hidden, float shown) { return hidden + (shown - hidden) * m_chrome; };
  if (wash.id != 0 && m_washPath != current) {
    // A new photo: its wash fades in over the old one (DESIGN.md: 300 ms cross-fade, no extra decode).
    m_oldWashPath = m_washPath;
    m_washPath = current;
    m_washFade = m_oldWashPath.empty() ? 1.0F : 0.0F;
    if (!m_oldWashPath.empty()) {
      m_washAnim = m_animations.drive(m_washAnim, 0.0F, 1.0F, Style::spring(Tokens::springFade), [this](float v) {
        m_washFade = std::clamp(v, 0.0F, 1.0F);
        refresh();
      }, [this] {
        m_washAnim = 0;
        m_oldWashPath.clear();
        refresh();
      }, this);
    }
  }
  m_ambient->setVisible(wash.id != 0);
  if (wash.id != 0) {
    m_ambient->setExternalTexture(renderer, wash);
    m_ambient->setOpacity(kAmbientOpacity * m_washFade);
    m_ambient->setPosition(-inset, -inset);
    m_ambient->setSize(w + inset + inset, h + inset + inset);
    m_ambient->layout(renderer);
  }
  const TextureHandle oldWash = m_oldWashPath.empty() ? TextureHandle{} : texture(m_oldWashPath, kAmbientPx);
  m_ambientOld->setVisible(oldWash.id != 0 && m_washFade < 1.0F);
  if (m_ambientOld->visible()) {
    m_ambientOld->setExternalTexture(renderer, oldWash);
    // Under the new wash (alpha a·f) the old one keeps the total coverage at a, so the stage shows no dip mid-fade:
    // 1 - (1 - a·f)(1 - o) = a  =>  o = a(1 - f) / (1 - a·f).
    const float newAlpha = kAmbientOpacity * m_washFade;
    m_ambientOld->setOpacity(kAmbientOpacity * (1.0F - m_washFade) / std::max(1e-3F, 1.0F - newAlpha));
    m_ambientOld->setPosition(-inset, -inset);
    m_ambientOld->setSize(w + inset + inset, h + inset + inset);
    m_ambientOld->layout(renderer);
  }
  // Speed budgets (DESIGN.md "Files v2" Gallery): next photo <= 16 ms from its cached thumbnail, full size <= 150 ms.
  const auto sinceAsked = [this] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_shownAt).count();
  };
  if (!m_loggedThumb && photo.id != 0) {
    m_loggedThumb = true;
    kLog.debug("gallery photo shown in {:.1f} ms", sinceAsked());
  }
  if (!m_loggedFull && texture(current, photoPx).id != 0) {
    m_loggedFull = true;
    kLog.debug("gallery photo full size in {:.1f} ms", sinceAsked());
  }
  // Zoomed, the fit ignores the chrome: its fade must not resize a photo the user is looking into.
  const float side = zoomed ? kEdge * s : mix(kEdge * s, inset);
  // Mockup .photo top 96: the title row plus a 40 gap.
  const float top = zoomed ? kEdge * s : mix(kEdge * s, (kEdge + kCapsuleH + Tokens::spaceP40) * s);
  const float bottom = zoomed ? kEdge * s : mix(kEdge * s, (kFilmH + kEdge + kEdge) * s);
  // The open Info card takes the right edge: the photo centres in what is left (mockup section 15).
  const float card = m_infoOpen ? (kInfoW + kEdge) * s : 0.0F;
  const float availW = std::max(1.0F, w - side - side - card);
  const float availH = std::max(1.0F, h - top - bottom);
  m_photo->setVisible(photo.id != 0);
  m_photoShadow->setVisible(photo.id != 0);
  if (photo.id == 0 && m_videoActive && m_host.placeVideo) {
    // A video with no poster frame yet: it fits the free stage, keeping its own aspect.
    float vx = 0.0F;
    float vy = 0.0F;
    Node::mapToScene(this, side, top, vx, vy);
    const std::array<float, 4> rect{vx, vy, availW, availH};
    if (rect != m_videoRect) {
      m_videoRect = rect;
      m_host.placeVideo(rect);
    }
  }
  if (photo.id != 0) {
    const float aspect = static_cast<float>(photo.width) / static_cast<float>(std::max(1, photo.height));
    float pw = availW;
    float ph = pw / aspect;
    if (ph > availH) {
      ph = availH;
      pw = ph * aspect;
    }
    m_fitW = pw;
    m_fitH = ph;
    m_stageCx = (w - card) / 2.0F;
    m_stageCy = top + availH / 2.0F;
    clampPan();
    const float zw = pw * m_zoom;
    const float zh = ph * m_zoom;
    m_photo->setExternalTexture(renderer, photo);
    m_photo->setPosition(std::round(m_stageCx + m_panX - zw / 2.0F), std::round(m_stageCy + m_panY - zh / 2.0F));
    m_photo->setSize(std::round(zw), std::round(zh));
    m_photo->layout(renderer);
    m_photoShadow->setPosition(m_photo->x(), m_photo->y() + Tokens::spaceP24 * s);
    m_photoShadow->setSize(m_photo->width(), m_photo->height());
    if (m_videoActive && m_host.placeVideo) {
      // The video subsurface covers the photo exactly; its first placement starts it.
      float vx = 0.0F;
      float vy = 0.0F;
      Node::mapToScene(m_photo, 0.0F, 0.0F, vx, vy);
      const std::array<float, 4> rect{vx, vy, m_photo->width(), m_photo->height()};
      if (rect != m_videoRect) {
        m_videoRect = rect;
        m_host.placeVideo(rect);
      }
    }
    if (zoomed && chrome) {
      // Navigator: the whole photo contained in 96 x 72, a frame on the part in view.
      float nw = kNavW * s;
      float nh = nw / aspect;
      if (nh > kNavH * s) {
        nh = kNavH * s;
        nw = nh * aspect;
      }
      const float nx = w - card - kEdge * s - nw;
      const float ny = h - kEdge * s - nh;
      m_navigator->setVisible(true);
      m_navigator->setExternalTexture(renderer, photo);
      m_navigator->setPosition(std::round(nx), std::round(ny));
      m_navigator->setSize(std::round(nw), std::round(nh));
      m_navigator->layout(renderer);
      const float left = m_stageCx + m_panX - zw / 2.0F;
      const float topEdge = m_stageCy + m_panY - zh / 2.0F;
      const float fx0 = std::clamp((0.0F - left) / zw, 0.0F, 1.0F);
      const float fx1 = std::clamp((w - card - left) / zw, 0.0F, 1.0F);
      const float fy0 = std::clamp((0.0F - topEdge) / zh, 0.0F, 1.0F);
      const float fy1 = std::clamp((h - topEdge) / zh, 0.0F, 1.0F);
      m_navigatorFrame->setVisible(true);
      m_navigatorFrame->setPosition(std::round(nx + fx0 * nw), std::round(ny + fy0 * nh));
      m_navigatorFrame->setSize(std::max(4.0F, std::round((fx1 - fx0) * nw)), std::max(4.0F, std::round((fy1 - fy0) * nh)));
      m_zoomLabel->setText(std::format("{} %", std::lround(m_zoom / zoom100() * 100.0F)));
      m_zoomChip->setVisible(true);
      const LayoutSize chipSize = m_zoomChip->measure(renderer, LayoutConstraints::available(w, Tokens::spaceP32 * s));
      m_zoomChip->arrange(renderer, LayoutRect{.x = kEdge * s, .y = h - (kEdge + Tokens::spaceP32) * s,
                                               .width = chipSize.width, .height = Tokens::spaceP32 * s});
    }
  }

  // Mockup .gt: the place once the Info read found one, else the file name.
  const bool placed = m_infoFor == m_items[m_current] && !m_info.place.empty();
  m_name->setText(placed ? m_info.place : m_items[m_current].filename().string());
  std::string position = std::format("{} of {}", m_current + 1, m_items.size());
  if (const auto t = static_cast<std::time_t>(m_current < m_itemInfo.size() ? m_itemInfo[m_current].time : 0); t != 0) {
    std::tm local{};
    std::array<char, 32> date{};
    if (localtime_r(&t, &local) != nullptr && std::strftime(date.data(), date.size(), "%-d %b %Y, %H:%M", &local) > 0) {
      position += std::format(" · {}", date.data());
    }
  }
  m_position->setText(position);
  m_name->setMaxWidth(w / 2.0F);
  const LayoutSize titleSize = m_title->measure(renderer, LayoutConstraints::available(w / 2.0F, kCapsuleH * s));
  m_title->arrange(renderer, LayoutRect{.x = kEdge * s, .y = kEdge * s, .width = titleSize.width, .height = kCapsuleH * s});

  const fs::path& item = m_items[m_current];
  const bool starred = m_host.starred && m_host.starred(item);
  if (m_starGlyph->setGlyph(starred ? "star-filled" : "star")) {
    kLog.debug("gallery star {}", starred ? "filled" : "outline"); // tests/e2e/files_gallery.sh
  }
  m_starGlyph->setColor(role(starred ? ColorRole::Warning : ColorRole::Label1));
  std::string ext = item.extension().string();
  std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  m_rotateButton->setVisible(ext == ".jpg" || ext == ".jpeg"); // lossless only: the EXIF Orientation tag
  m_phoneButton->setVisible(m_host.phoneName && !m_host.phoneName().empty());
  m_infoButton->setFill(m_infoOpen ? role(ColorRole::Fill2) : ColorSpec{});
  // The Info card (280 + edge) sits under the capsule's right end when open (mockup: right 312).
  const float actionsRight = kEdge * s + (m_infoOpen ? (kInfoW + kEdge) * s : 0.0F);
  const LayoutSize actionsSize = m_actions->measure(renderer, LayoutConstraints::available(w, kCapsuleH * s));
  m_actions->arrange(renderer, LayoutRect{.x = w - actionsRight - actionsSize.width, .y = kEdge * s,
                                          .width = actionsSize.width, .height = kCapsuleH * s});

  m_infoCard->setVisible(m_infoOpen && chrome && !m_info.name.empty() && m_infoFor == item);
  m_infoCard->setOpacity(m_chrome);
  if (m_infoCard->visible()) {
    const LayoutSize cardSize = m_infoCard->measure(renderer, LayoutConstraints::available(kInfoW * s, h));
    kLog.debug("gallery info card {}x{}", cardSize.width, cardSize.height);
    m_infoCard->arrange(renderer, LayoutRect{.x = w - (kEdge + kInfoW) * s, .y = (kEdge + kCapsuleH + kEdge) * s,
                                             .width = kInfoW * s, .height = cardSize.height});
    if (m_map != nullptr && m_map->parent() != nullptr) {
      if (m_mapDirty) {
        m_map->setSourceRaw(renderer, m_info.mapBgra.data(), m_info.mapBgra.size(), m_info.mapWidth, m_info.mapHeight,
                            m_info.mapWidth * 4, PixmapFormat::BGRA);
        m_mapDirty = false;
      }
      m_map->setPosition(0.0F, 0.0F);
      m_map->setSize(m_map->parent()->width(), m_map->parent()->height());
      m_map->layout(renderer);
    }
  }

  const float arrowSide = kCapsuleH * s;
  const float arrowY = std::round((h - arrowSide) / 2.0F);
  m_prev->arrange(renderer, LayoutRect{.x = kEdge * s, .y = arrowY, .width = arrowSide, .height = arrowSide});
  if (m_scrub->visible()) {
    m_scrubGlyph->setGlyph(m_scrubPlaying ? "player-pause" : "player-play");
    const float card = m_infoOpen ? (kInfoW + kEdge) * s : 0.0F;
    const float scrubW = std::max(1.0F, w - card - kEdge * s * 2.0F);
    m_scrub->arrange(renderer, LayoutRect{.x = kEdge * s, .y = h - (kEdge + kCapsuleH) * s, .width = scrubW,
                                          .height = kCapsuleH * s});
    m_scrubLevel->setPosition(0.0F, 0.0F);
    m_scrubLevel->setSize(std::round(m_scrubTrack->width() * m_scrubProgress), m_scrubTrack->height());
  }
  // Left of an open Info card.
  const float nextX = w - kEdge * s - arrowSide - (m_infoOpen ? (kInfoW + kEdge) * s : 0.0F);
  m_next->arrange(renderer, LayoutRect{.x = nextX, .y = arrowY, .width = arrowSide, .height = arrowSide});

  for (std::size_t k = 0; k < m_filmImages.size(); ++k) {
    const TextureHandle t = texture(m_items[m_filmIndex[k]].string(), kFilmPx);
    if (t.id == 0) {
      continue;
    }
    m_filmImages[k]->setExternalTexture(renderer, t);
    if (m_filmIndex[k] == m_current && t.height > 0) {
      // The current photo at its own aspect (mockup .film i.on), 32 to 96 wide on the 4 grid.
      const float aspect = static_cast<float>(t.width) / static_cast<float>(t.height);
      const float cw = std::clamp(std::round(kFilmItemH * aspect / 4.0F) * 4.0F, kFilmItemW, 96.0F);
      if (cw != m_filmCurrentW) {
        m_filmCurrentW = cw;
        auto* cell = static_cast<Flex*>(m_filmImages[k]->parent());
        cell->setMinWidth(cw * s);
        cell->setMaxWidth(cw * s);
        m_filmImages[k]->setSize(cw * s, kFilmItemH * s);
      }
    }
  }
  // Fixed-size children: the strip is their sum, the gaps and the padding (measure undercounts fixed cells).
  const auto& filmChildren = m_film->children();
  float filmW = Tokens::spaceP8 * s * 2.0F;
  for (std::size_t k = 0; k < filmChildren.size(); ++k) {
    const auto* cell = dynamic_cast<const Flex*>(filmChildren[k].get());
    filmW += (cell != nullptr ? cell->minWidth() : std::max(1.0F, s)) + (k > 0 ? Tokens::spaceP4 * s : 0.0F);
  }
  filmW = std::min(filmW, w - kEdge * s * 2.0F);
  const float filmX = std::round((w - filmW) / 2.0F);
  const float filmY = h - kEdge * s - kFilmH * s;
  m_film->arrange(renderer, LayoutRect{.x = filmX, .y = filmY, .width = filmW, .height = kFilmH * s});
  for (std::size_t k = 0; k < m_filmIndex.size(); ++k) {
    if (m_filmIndex[k] != m_current) {
      continue;
    }
    const Node* cell = m_filmImages[k]->parent();
    m_filmRing->setPosition(filmX + cell->x(), filmY + cell->y());
    m_filmRing->setSize(cell->width(), cell->height());
  }
}
