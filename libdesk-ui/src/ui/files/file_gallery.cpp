#include "ui/files/file_gallery.h"

#include "cursor-shape-v1-client-protocol.h"

#include "core/log.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("file-gallery");
  // The Gallery follows the scheme: window background stage, popover capsules with a stroke, label text.
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
  const float& kFilmCurrentW = Tokens::spaceP64;
  const float& kEdge = Tokens::spaceP16;


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

  std::unique_ptr<Capsule> capsule(float s, float radius) {
    auto c = std::make_unique<Capsule>();
    c->setDirection(FlexDirection::Horizontal);
    c->setAlign(FlexAlign::Center);
    c->setFill(role(ColorRole::Popover));
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
} // namespace

FileGallery::FileGallery(float scale, ThumbnailService* thumbnails, AnimationManager& animations, Host host)
    : m_scale(scale), m_thumbs(thumbnails), m_host(std::move(host)), m_animations(animations) {
  const float s = scale;
  m_bg = static_cast<Box*>(addChild(ui::box({.fill = role(ColorRole::SolidRegular)})));
  m_ambient = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_ambient->setFit(ImageFit::Cover);
  m_ambient->setOpacity(kAmbientOpacity);
  m_photo = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_photo->setFit(ImageFit::Contain);
  m_photo->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
  // Under the capsules: a move anywhere brings the chrome back and restarts its idle time.
  auto hover = ui::inputArea({.participatesInLayout = false});
  hover->setOnEnter([this](const InputArea::PointerData&) { showChrome(); });
  hover->setOnMotion([this](const InputArea::PointerData&) { showChrome(); });
  hover->setAcceptedButtons(0); // presses and wheel steps go on to the stage (menus, zoom)
  hover->setAcceptedScrollDirections(0);
  m_hover = static_cast<InputArea*>(addChild(std::move(hover)));

  auto title = capsule(s, kCapsuleH * s / 2.0F);
  title->setDirection(FlexDirection::Vertical);
  title->setAlign(FlexAlign::Start);
  title->setJustify(FlexJustify::Center);
  title->setPadding(0.0F, Tokens::spaceP16 * s);
  title->setMinHeight(kCapsuleH * s);
  title->setMaxHeight(kCapsuleH * s);
  title->addChild(ui::label({.out = &m_name, .fontSize = Style::fontSizeBody * s, .fontWeight = FontWeight::SemiBold,
                             .color = role(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::Middle}));
  title->addChild(ui::label({.out = &m_position, .fontSize = Style::fontSizeCaption * s, .color = role(ColorRole::Label2),
                             .maxLines = 1}));
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
}

FileGallery::~FileGallery() {
  m_animations.cancel(m_chromeAnim);
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

void FileGallery::setItems(std::vector<fs::path> items, std::size_t current) {
  m_items = std::move(items);
  m_current = m_items.empty() ? 0 : std::min(current, m_items.size() - 1);
  m_filmDirty = true; // the filmstrip window moves with the current photo
  showChrome();
  markLayoutDirty();
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

void FileGallery::hold(const std::string& path, int px, std::vector<Held>& next) {
  if (m_thumbs == nullptr) {
    return;
  }
  (void)m_thumbs->acquire(path, px);
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
  while (!m_film->children().empty()) {
    (void)m_film->removeChild(m_film->children().back().get());
  }
  m_filmImages.clear();
  m_filmIndex.clear();
  const float s = m_scale;
  const std::size_t first = m_current > kFilmSide ? m_current - kFilmSide : 0;
  const std::size_t last = std::min(m_items.size(), m_current + kFilmSide + 1);
  for (std::size_t i = first; i < last; ++i) {
    auto cell = std::make_unique<Capsule>();
    const float w = (i == m_current ? kFilmCurrentW : kFilmItemW) * s;
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
    auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .participatesInLayout = false});
    area->setOnClick([this, i](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT && m_host.select) {
        m_host.select(i);
      }
    });
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
  for (Node* n : {static_cast<Node*>(m_title), static_cast<Node*>(m_film), static_cast<Node*>(m_filmRing),
                  static_cast<Node*>(m_prev), static_cast<Node*>(m_next)}) {
    n->setOpacity(m_chrome);
  }
  for (Node* n : {static_cast<Node*>(m_title), static_cast<Node*>(m_film), static_cast<Node*>(m_filmRing)}) {
    n->setVisible(any && chrome);
  }
  m_prev->setVisible(any && chrome && m_current > 0);
  m_next->setVisible(any && chrome && m_current + 1 < m_items.size());
  if (!any) {
    m_photo->setVisible(false);
    m_ambient->setVisible(false);
    releaseAll(m_held);
    return;
  }

  // Textures: the photo at the stage size, its 64 px wash, the filmstrip crops; the old holds go after the new ones.
  std::vector<Held> next;
  const std::string current = m_items[m_current].string();
  const int photoPx = std::min(kPhotoMaxPx, static_cast<int>(std::ceil(std::max(w, h) * s)));
  hold(current, photoPx, next);
  hold(current, kAmbientPx, next);
  for (std::size_t k = 0; k < m_filmIndex.size(); ++k) {
    hold(m_items[m_filmIndex[k]].string(), kFilmPx, next);
  }
  releaseAll(m_held);
  m_held = std::move(next);

  const auto texture = [this](const std::string& path, int px) {
    return m_thumbs != nullptr ? m_thumbs->peek(path, px) : TextureHandle{};
  };
  TextureHandle photo = texture(current, photoPx);
  const TextureHandle wash = texture(current, kAmbientPx);
  if (photo.id == 0) {
    photo = wash; // the cached small one at once, the full size swaps in (DESIGN.md speed)
  }

  const float inset = Tokens::spaceP64 * s;
  const auto mix = [this](float hidden, float shown) { return hidden + (shown - hidden) * m_chrome; };
  m_ambient->setVisible(wash.id != 0);
  if (wash.id != 0) {
    m_ambient->setExternalTexture(renderer, wash);
    m_ambient->setPosition(-inset, -inset);
    m_ambient->setSize(w + inset + inset, h + inset + inset);
    m_ambient->layout(renderer);
  }
  m_photo->setVisible(photo.id != 0);
  if (photo.id != 0) {
    const float side = mix(kEdge * s, inset);
    const float top = mix(kEdge * s, inset);
    const float bottom = mix(kEdge * s, (kFilmH + kEdge + kEdge) * s);
    const float availW = std::max(1.0F, w - side - side);
    const float availH = std::max(1.0F, h - top - bottom);
    const float aspect = static_cast<float>(photo.width) / static_cast<float>(std::max(1, photo.height));
    float pw = availW;
    float ph = pw / aspect;
    if (ph > availH) {
      ph = availH;
      pw = ph * aspect;
    }
    m_photo->setExternalTexture(renderer, photo);
    m_photo->setPosition(std::round((w - pw) / 2.0F), std::round(top + (availH - ph) / 2.0F));
    m_photo->setSize(std::round(pw), std::round(ph));
    m_photo->layout(renderer);
  }

  m_name->setText(m_items[m_current].filename().string());
  m_position->setText(std::format("{} of {}", m_current + 1, m_items.size()));
  m_name->setMaxWidth(w / 2.0F);
  const LayoutSize titleSize = m_title->measure(renderer, LayoutConstraints::available(w / 2.0F, kCapsuleH * s));
  m_title->arrange(renderer, LayoutRect{.x = kEdge * s, .y = kEdge * s, .width = titleSize.width, .height = kCapsuleH * s});

  const float arrowSide = kCapsuleH * s;
  const float arrowY = std::round((h - arrowSide) / 2.0F);
  m_prev->arrange(renderer, LayoutRect{.x = kEdge * s, .y = arrowY, .width = arrowSide, .height = arrowSide});
  m_next->arrange(renderer, LayoutRect{.x = w - kEdge * s - arrowSide, .y = arrowY, .width = arrowSide, .height = arrowSide});

  for (std::size_t k = 0; k < m_filmImages.size(); ++k) {
    const TextureHandle t = texture(m_items[m_filmIndex[k]].string(), kFilmPx);
    if (t.id != 0) {
      m_filmImages[k]->setExternalTexture(renderer, t);
    }
  }
  // The cells' widths are known: the strip is their sum, the gaps and the padding (measure undercounts fixed cells).
  float filmW = Tokens::spaceP8 * s * 2.0F;
  for (std::size_t k = 0; k < m_filmIndex.size(); ++k) {
    filmW += (m_filmIndex[k] == m_current ? kFilmCurrentW : kFilmItemW) * s + (k > 0 ? Tokens::spaceP4 * s : 0.0F);
  }
  filmW = std::min(filmW, w - kEdge * s * 2.0F);
  const float filmX = std::round((w - filmW) / 2.0F);
  const float filmY = h - kEdge * s - kFilmH * s;
  m_film->arrange(renderer, LayoutRect{.x = filmX, .y = filmY, .width = filmW, .height = kFilmH * s});
  for (std::size_t k = 0; k < m_filmIndex.size(); ++k) {
    if (m_filmIndex[k] != m_current) {
      continue;
    }
    const Node* cell = m_film->children()[k].get();
    m_filmRing->setPosition(filmX + cell->x(), filmY + cell->y());
    m_filmRing->setSize(cell->width(), cell->height());
  }
}
