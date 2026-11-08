#include "scene/ssd_frame.h"

#include "config/config.h"
#include "design_tokens.h"
#include "scene/text_buffer.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "output/output.h"
#include "server/server.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {
    // Frames with a hover fade running; a frame leaves the list when it rests or is destroyed.
    std::vector<SsdFrame*> g_hoverFading;
    TitleBarMotion* g_titleBarMotion = nullptr;

    void startHoverFade(SsdFrame* frame) {
      if (std::ranges::find(g_hoverFading, frame) == g_hoverFading.end()) {
        g_hoverFading.push_back(frame);
      }
    }
  } // namespace

  TitleBarMotion::TitleBarMotion(Server& server) : m_server(server) {
    g_titleBarMotion = this;
    m_server.registerAnimatable(this);
  }

  TitleBarMotion::~TitleBarMotion() {
    m_server.unregisterAnimatable(this);
    g_titleBarMotion = nullptr;
  }

  bool TitleBarMotion::tickAnimations(double nowMsec) {
    // A tick can end a fade, which drops the frame from the list.
    const std::vector<SsdFrame*> frames = g_hoverFading;
    for (SsdFrame* frame : frames) {
      if (!frame->tickHover(nowMsec)) {
        std::erase(g_hoverFading, frame);
      }
    }
    return !g_hoverFading.empty();
  }

  void TitleBarMotion::scheduleFrames() {
    for (const auto& output : m_server.outputs()) {
      wlr_output_schedule_frame(output->wlr());
    }
  }

  bool TitleBarMotion::hasActiveAnimations() const { return !g_hoverFading.empty(); }

  // Frames do not know their output, so every output keeps drawing while a fade (about 150 ms) runs.
  bool TitleBarMotion::animatesOn(const Output* output) const { return output != nullptr && hasActiveAnimations(); }

  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }

    std::array<float, 4> rgba(std::uint32_t value) {
      const auto channel = [value](int shift) { return static_cast<float>((value >> shift) & 0xFFU) / 255.0F; };
      return {channel(24), channel(16), channel(8), channel(0)};
    }

    bool lightScheme() { return config().appearance.scheme == ColorScheme::Light; }
    const auto& scheme() { return lightScheme() ? Tokens::schemeLight : Tokens::schemeDark; }

    struct Palette {
      std::array<float, 4> background, backgroundUnfocused, text, textUnfocused;
    };

    // Untouched title-bar colours follow the light or dark scheme (header, label_1, inactive label_3); a theme template or
    // the user's own colours win.
    Palette palette() {
      const auto& colors = config().colors.titleBar;
      if (!(colors == std::remove_cvref_t<decltype(colors)>{})) {
        return {colors.background, colors.backgroundUnfocused, colors.text, colors.textUnfocused};
      }
      const auto& s = scheme();
      return {rgba(s.header), rgba(s.header), rgba(s.label1), rgba(s.label3)};
    }

    // A button glyph drawn into `px` device pixels: Windows lines fill the box, macOS marks sit inside the light.
    wlr_buffer* glyphBuffer(TitleBarButton button, bool mac, int px, const std::array<float, 4>& color, double scale) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
      cairo_t* cr = cairo_create(surface);
      cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
      const double line = std::max(1.0, std::round(scale));
      cairo_set_line_width(cr, line);
      const double lo = mac ? px * 0.3 : line / 2.0;
      const double hi = px - lo;
      const double mid = std::floor(px / 2.0) + (static_cast<int>(line) % 2 == 1 ? 0.5 : 0.0);
      switch (button) {
      case TitleBarButton::Minimize:
        cairo_move_to(cr, lo, mid);
        cairo_line_to(cr, hi, mid);
        break;
      case TitleBarButton::Maximize:
        if (mac) {
          cairo_move_to(cr, lo, mid);
          cairo_line_to(cr, hi, mid);
          cairo_move_to(cr, mid, lo);
          cairo_line_to(cr, mid, hi);
        } else {
          cairo_rectangle(cr, lo, lo, hi - lo, hi - lo);
        }
        break;
      case TitleBarButton::Close:
        cairo_move_to(cr, lo, lo);
        cairo_line_to(cr, hi, hi);
        cairo_move_to(cr, hi, lo);
        cairo_line_to(cr, lo, hi);
        break;
      }
      cairo_stroke(cr);
      cairo_destroy(cr);
      cairo_surface_flush(surface);
      wlr_buffer* buffer = imageBuffer(surface);
      cairo_surface_destroy(surface);
      return buffer;
    }
  } // namespace

  SsdFrame::SsdFrame(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels, wlr_scene_tree* inputParent)
      : m_tree(wlr_scene_tree_create(parent)),
        m_gripTree(wlr_scene_tree_create(inputParent != nullptr ? inputParent : parent)), m_labels(std::move(labels)) {
    if (inputParent != nullptr && parent->node.parent == inputParent) {
      // Client surfaces and popups keep input priority over invisible resize grips.
      wlr_scene_node_place_below(&m_gripTree->node, &parent->node);
    }
    const float clear[4]{};
    m_background = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_separator = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_hoverCell = wlr_scene_rect_create(m_tree, 1, 1, clear);
    wlr_scene_node_set_enabled(&m_hoverCell->node, false);
    m_close = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_maximize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_minimize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    for (auto*& glyph : m_glyphs) {
      glyph = wlr_scene_buffer_create(m_tree, nullptr);
      glyph->point_accepts_input = rejectInput;
      wlr_scene_node_set_enabled(&glyph->node, false);
    }
    for (auto*& grip : m_grips)
      grip = wlr_scene_rect_create(m_gripTree, 1, 1, clear);
    wlr_scene_node_set_enabled(&m_tree->node, false);
    wlr_scene_node_set_enabled(&m_gripTree->node, false);
  }

  SsdFrame::~SsdFrame() {
    std::erase(g_hoverFading, this);
    if (m_blur != nullptr)
      wlr_scene_node_destroy(&m_blur->node);
    wlr_scene_node_destroy(&m_gripTree->node);
    wlr_scene_node_destroy(&m_tree->node);
  }

  void SsdFrame::layout(const ViewChromeGeometry& geometry) {
    if (m_geometry == geometry)
      return;
    m_geometry = geometry;
    redraw();
  }
  void SsdFrame::setFocused(bool focused) {
    if (m_focused == focused)
      return;
    m_focused = focused;
    redraw();
  }
  void SsdFrame::setAlpha(float alpha) { setAlphas(alpha, alpha); }
  void SsdFrame::setAlphas(float chromeAlpha, float blurAlpha) {
    chromeAlpha = std::clamp(chromeAlpha, 0.0F, 1.0F);
    blurAlpha = std::clamp(blurAlpha, 0.0F, 1.0F);
    if (m_alpha == chromeAlpha && m_blurAlpha == blurAlpha)
      return;
    m_alpha = chromeAlpha;
    m_blurAlpha = blurAlpha;
    updateColors();
  }
  void SsdFrame::reloadConfig() { redraw(); }
  void SsdFrame::setTitle(std::string_view title) {
    if (m_title == title)
      return;
    m_title = title;
    redraw();
  }
  void SsdFrame::setResizable(bool resizable) {
    if (m_resizable == resizable)
      return;
    m_resizable = resizable;
    redraw();
  }
  void SsdFrame::setMaximizeEnabled(bool enabled) {
    if (m_maximizeEnabled == enabled)
      return;
    m_maximizeEnabled = enabled;
    updateColors();
  }
  void SsdFrame::setButtons(std::span<const TitleBarButton> buttons) {
    if (std::ranges::equal(m_buttons, buttons))
      return;
    m_buttons.assign(buttons.begin(), buttons.end());
    redraw();
  }
  void SsdFrame::setMaximized(bool maximized) { m_maximized = maximized; }
  void SsdFrame::setHovered(SsdFramePart part, bool fade) {
    if (m_hovered == part)
      return;
    m_hovered = part;
    // P3: the wash comes in on fade_out (quick) and leaves on fade (slower); between two buttons it moves at once.
    const bool animate = fade && config().animation.enabled && !m_preview && g_titleBarMotion != nullptr;
    if (part != SsdFramePart::None) {
      m_shownHover = part;
    }
    const double target = part != SsdFramePart::None ? 1.0 : 0.0;
    if (animate && m_hoverLevel.current() != target) {
      const auto& spring = target > 0.0 ? Tokens::springFadeOut : Tokens::springFade;
      m_hoverLevel.settleSpring(target, {.damping = spring.damping, .stiffness = spring.stiffness});
      startHoverFade(this);
      g_titleBarMotion->scheduleFrames();
    } else {
      m_hoverLevel.snap(target);
      if (target == 0.0) {
        m_shownHover = SsdFramePart::None;
      }
    }
    updateColors();
  }

  bool SsdFrame::tickHover(double nowMsec) {
    // tick() also answers true on the step that settles; the frame leaves the list only once nothing moves.
    m_hoverLevel.tick(nowMsec);
    const bool running = m_hoverLevel.animating();
    if (!running && m_hoverLevel.current() <= 0.0) {
      m_shownHover = SsdFramePart::None;
    }
    updateColors();
    return running;
  }
  void SsdFrame::setPressed(SsdFramePart part) {
    if (m_pressed == part)
      return;
    m_pressed = part;
    updateColors();
  }

  void SsdFrame::setBlurOptions(const SurfaceBlurOptions& options) {
    m_blurOptions = options;
    updateBlur();
  }
  void SsdFrame::setBlurLive(bool live) {
    m_blurLive = live;
    updateBlur();
  }

  void SsdFrame::updateBlur() {
    const auto& cfg = config().appearance.blur;
    const Palette tones = palette();
    const float backgroundAlpha = (m_focused ? tones.background : tones.backgroundUnfocused)[3];
    // The title bar has its own transparency, independent of the client's opaque region or alpha mask.
    const bool enabled = m_tree->node.enabled
        && cfg.enabled
        && m_blurOptions.enabled.value_or(false)
        && backgroundAlpha * m_alpha < 1.0F
        && backgroundAlpha >= m_blurOptions.ignoreAlpha;
    if (!enabled) {
      if (m_blur != nullptr)
        wlr_scene_node_set_enabled(&m_blur->node, false);
      return;
    }
    if (m_blur == nullptr) {
      // Blur the backdrop below the window, excluding the window's own client content near the title bar seam.
      m_blur = wlr_scene_blur_create(m_tree->node.parent, m_background->width, m_background->height);
      if (m_blur == nullptr)
        return;
      wlr_scene_node_lower_to_bottom(&m_blur->node);
    }
    wlr_scene_node_set_enabled(&m_blur->node, true);
    wlr_scene_node_set_position(
        &m_blur->node, m_tree->node.x + m_background->node.x, m_tree->node.y + m_background->node.y
    );
    wlr_scene_blur_set_size(m_blur, m_background->width, m_background->height);
    wlr_scene_blur_set_corner_radii(m_blur, m_background->corners);
    wlr_scene_blur_set_should_only_blur_bottom_layer(
        m_blur, m_blurOptions.optimized.value_or(cfg.optimized) && !m_blurLive
    );
    wlr_scene_blur_set_alpha(m_blur, m_blurAlpha);
    const glass::Material material = configGlass(glass::Kind::Sheet);
    wlr_scene_blur_set_liquid(m_blur, material.level);
    wlr_scene_blur_set_rim(m_blur, material.rim);
    wlr_scene_blur_set_strength(m_blur, material.blurStrength);
  }

  int SsdFrame::zoomed(int value) const { return static_cast<int>(std::lround(value * m_geometry.zoom)); }

  void SsdFrame::updateColors() {
    const auto& colors = config().colors.titleBar;
    const TitleBarStyle style = config().appearance.titleBar.style;
    const Palette tones = palette();
    const auto& tokens = scheme();
    float color[4];
    const auto& background = m_focused ? tones.background : tones.backgroundUnfocused;
    premultiplied(color, background, m_alpha);
    wlr_scene_rect_set_color(m_background, color);
    premultiplied(color, rgba(tokens.separator), m_alpha);
    wlr_scene_rect_set_color(m_separator, color);
    const auto level = static_cast<float>(std::clamp(m_hoverLevel.current(), 0.0, 1.0));
    const bool buttonHovered = level > 0.0F
        && (m_shownHover == SsdFramePart::Close
            || m_shownHover == SsdFramePart::Maximize
            || m_shownHover == SsdFramePart::Minimize);
    const auto button = [&](wlr_scene_rect* rect, std::array<float, 4> value, SsdFramePart part, bool enabled) {
      if (style == TitleBarStyle::Windows) {
        // Windows draws glyphs only; the round marks stay hidden.
        wlr_scene_node_set_enabled(&rect->node, false);
        return;
      }
      // Lights at full strength; an inactive window shows all three in fill_3.
      const float alpha = enabled ? 1.0F : 0.35F;
      if (!m_focused) {
        value = rgba(tokens.fill3);
      }
      if (enabled && m_pressed == part) {
        for (size_t i = 0; i < 3; ++i)
          value[i] *= 0.8F;
      }
      premultiplied(color, value, m_alpha * alpha);
      wlr_scene_rect_set_color(rect, color);
    };
    const bool mac = style == TitleBarStyle::Macos;
    // Untouched Andromeda lights: minimize #f5c824, maximize accent, close danger.
    const bool ownColors = colors == std::remove_cvref_t<decltype(colors)>{};
    button(m_close, mac ? rgba(0xff5f57ffU) : ownColors ? rgba(tokens.danger) : colors.close, SsdFramePart::Close, true);
    button(
        m_maximize, mac ? rgba(0x28c840ffU) : ownColors ? rgba(tokens.accent) : colors.maximize, SsdFramePart::Maximize,
        m_maximizeEnabled
    );
    button(
        m_minimize, mac ? rgba(0xfebc2effU) : ownColors ? rgba(0xf5c824ffU) : colors.minimize, SsdFramePart::Minimize,
        true
    );

    // Glyphs: Windows always (label_1, white on the close fill); lights only while the group is hovered.
    const double scale = std::max(1.0, std::ceil(static_cast<double>(m_geometry.scale)));
    constexpr std::array kButtons{TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
    constexpr std::array kParts{SsdFramePart::Minimize, SsdFramePart::Maximize, SsdFramePart::Close};
    for (size_t i = 0; i < kButtons.size(); ++i) {
      wlr_scene_buffer* glyph = m_glyphs[static_cast<size_t>(kButtons[i])];
      if (glyph == nullptr) {
        continue;
      }
      const auto& box = m_parts.button(kButtons[i]);
      const bool shown = box.has_value() && m_geometry.titleBarHeight > 0
          && (style == TitleBarStyle::Windows || (m_focused && buttonHovered));
      wlr_scene_node_set_enabled(&glyph->node, shown);
      if (!shown) {
        continue;
      }
      const bool enabled = kButtons[i] != TitleBarButton::Maximize || m_maximizeEnabled;
      const bool lights = style != TitleBarStyle::Windows;
      const std::array<float, 4> ink = lights ? std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.6F}
          : !enabled                 ? rgba(tokens.label3)
          : (m_shownHover == SsdFramePart::Close && kParts[i] == SsdFramePart::Close && level > 0.5F)
          ? std::array<float, 4>{1, 1, 1, 1}
                                                                                    : rgba(tokens.label1);
      // Redrawn only when its look changes; fades only change the node's opacity.
      const int px = static_cast<int>(std::lround(box->width * scale));
      const GlyphKey key{.px = px, .mac = lights, .ink = ink, .scale = scale};
      if (m_glyphKeys[static_cast<size_t>(kButtons[i])] != key) {
        m_glyphKeys[static_cast<size_t>(kButtons[i])] = key;
        wlr_buffer* buffer = glyphBuffer(kButtons[i], lights, px, ink, scale);
        wlr_scene_buffer_set_buffer(glyph, buffer);
        if (buffer != nullptr) {
          wlr_buffer_drop(buffer);
        }
      }
      // Light glyphs come in with the hover wash; Windows glyphs always show.
      wlr_scene_buffer_set_opacity(glyph, lights ? m_alpha * level : m_alpha);
      wlr_scene_buffer_set_dest_size(glyph, std::max(1, zoomed(box->width)), std::max(1, zoomed(box->height)));
      wlr_scene_node_set_position(&glyph->node, zoomed(box->x), zoomed(box->y - m_geometry.titleBarHeight));
      wlr_scene_node_raise_to_top(&glyph->node);
    }

    const auto hoveredCell = [&]() -> std::optional<wlr_box> {
      switch (level > 0.0F ? m_shownHover : SsdFramePart::None) {
      case SsdFramePart::Close:
        return m_parts.cells[static_cast<size_t>(TitleBarButton::Close)];
      case SsdFramePart::Maximize:
        return m_maximizeEnabled ? m_parts.cells[static_cast<size_t>(TitleBarButton::Maximize)] : std::nullopt;
      case SsdFramePart::Minimize:
        return m_parts.cells[static_cast<size_t>(TitleBarButton::Minimize)];
      default:
        return std::nullopt;
      }
    }();
    const bool cellShown = style == TitleBarStyle::Windows && hoveredCell.has_value() && m_geometry.titleBarHeight > 0;
    wlr_scene_node_set_enabled(&m_hoverCell->node, cellShown);
    if (cellShown) {
      const int h = m_geometry.titleBarHeight;
      wlr_scene_node_set_position(&m_hoverCell->node, zoomed(hoveredCell->x), zoomed(-h));
      wlr_scene_rect_set_size(
          m_hoverCell, std::max(1, zoomed(hoveredCell->x + hoveredCell->width) - zoomed(hoveredCell->x)),
          std::max(1, zoomed(h))
      );
      // A cell at the bar's end follows the window's top corner there.
      const int radius = zoomed(nestedRadius(m_geometry.cornerRadius, m_geometry.borderInset));
      wlr_scene_rect_set_corner_radii(
          m_hoverCell,
          corner_radii_new(
              hoveredCell->x <= 0 ? radius : 0,
              hoveredCell->x + hoveredCell->width >= m_geometry.contentWidth ? radius : 0, 0, 0
          )
      );
      // Close fills with danger under a white glyph; minimize and maximize with fill_2.
      premultiplied(color, rgba(m_shownHover == SsdFramePart::Close ? tokens.danger : tokens.fill2), m_alpha * level);
      wlr_scene_rect_set_color(m_hoverCell, color);
    }
    if (m_label != nullptr)
      wlr_scene_buffer_set_opacity(m_label, m_alpha);
    updateBlur();
  }

  void SsdFrame::redraw() {
    const auto& g = m_geometry;
    const int h = g.titleBarHeight;
    const bool available = !g.suppressed && g.contentWidth > 0 && g.contentHeight > 0;
    const bool shown = available && h > 0;
    wlr_scene_node_set_enabled(&m_tree->node, shown);
    wlr_scene_node_set_enabled(&m_gripTree->node, available && m_resizable && !m_preview);
    if (!available) {
      updateBlur();
      return;
    }
    wlr_scene_node_set_position(&m_tree->node, zoomed(g.contentX), zoomed(g.contentY));
    wlr_scene_node_set_position(&m_gripTree->node, zoomed(g.contentX), zoomed(g.contentY));
    const auto place = [&](wlr_scene_rect* rect, const wlr_box& box) {
      wlr_scene_node_set_position(&rect->node, zoomed(box.x), zoomed(box.y));
      wlr_scene_rect_set_size(
          rect, std::max(1, zoomed(box.x + box.width) - zoomed(box.x)),
          std::max(1, zoomed(box.y + box.height) - zoomed(box.y))
      );
      rect->accepts_input = !m_preview;
    };
    const int band = g.borderInset + kSsdResizeBand;
    const std::array<wlr_box, 4> grips{{
        {-band, -h - band, g.contentWidth + 2 * band, band},
        {-band, g.contentHeight, g.contentWidth + 2 * band, band},
        {-band, -h, band, g.contentHeight + h},
        {g.contentWidth, -h, band, g.contentHeight + h},
    }};
    for (size_t i = 0; i < m_grips.size(); ++i) {
      place(m_grips[i], grips[i]);
      wlr_scene_node_set_enabled(&m_grips[i]->node, m_resizable && !m_preview);
    }
    if (!shown) {
      updateBlur();
      return;
    }
    place(m_background, {0, -h, g.contentWidth, h});
    place(m_separator, {0, -1, g.contentWidth, 1});
    const int radius = nestedRadius(g.cornerRadius, g.borderInset);
    wlr_scene_rect_set_corner_radii(m_background, corner_radii_new(zoomed(radius), zoomed(radius), 0, 0));
    const auto& style = config().appearance.titleBar;
    m_parts = titleBarLayout(g.contentWidth, h, style.buttonPosition, m_buttons, style.style);
    const TitleBarLayout& parts = m_parts;
    const auto drawButton = [&](wlr_scene_rect* button, const std::optional<wlr_box>& slot) {
      if (!slot) {
        wlr_scene_node_set_enabled(&button->node, false);
        return;
      }
      wlr_box box = *slot;
      box.y -= h;
      place(button, box);
      const int buttonRadius = style.buttonCornerRadius >= 0 ? style.buttonCornerRadius : g.cornerRadius;
      wlr_scene_rect_set_corner_radius(button, std::min(zoomed(box.width) / 2, zoomed(buttonRadius)));
      wlr_scene_node_set_enabled(&button->node, box.x >= 0 && box.x + box.width <= g.contentWidth);
    };
    drawButton(m_close, parts.button(TitleBarButton::Close));
    drawButton(m_maximize, parts.button(TitleBarButton::Maximize));
    drawButton(m_minimize, parts.button(TitleBarButton::Minimize));
    const Palette tones = palette();
    const TabLabel* label = m_labels->label({
        .text = m_title,
        .color = m_focused ? tones.text : tones.textUnfocused,
        .font = style.font,
        .maxWidth = parts.title.width,
        .scale = std::max(1.0, std::ceil(static_cast<double>(g.scale))),
    });
    if (label == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, label->buffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, label->buffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(label->width)), std::max(1, zoomed(label->height)));
      const int labelX = parts.centredTitle ? parts.title.x + std::max(0, (parts.title.width - label->width) / 2)
                                            : parts.title.x;
      wlr_scene_node_set_position(&m_label->node, zoomed(labelX), zoomed(-h + (h - label->height) / 2));
    }
    updateColors();
  }

  bool SsdFrame::contains(const wlr_scene_node* node) const {
    for (const wlr_scene_node* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == &m_tree->node || current == &m_gripTree->node)
        return true;
    }
    return false;
  }
  std::optional<wlr_box> SsdFrame::buttonCell(TitleBarButton button) const {
    auto cell = m_parts.cells[static_cast<size_t>(button)];
    if (cell) {
      cell->y -= m_geometry.titleBarHeight;
    }
    return cell;
  }

  SsdFramePoint SsdFrame::pointAt(double x, double y) const {
    if (m_preview || m_geometry.suppressed || (!m_tree->node.enabled && !m_gripTree->node.enabled))
      return {};
    return ssdFramePointAt(
        m_geometry.contentWidth, m_geometry.contentHeight, m_geometry.titleBarHeight, m_geometry.borderInset,
        m_resizable, config().appearance.titleBar.buttonPosition, m_buttons, x, y, config().appearance.titleBar.style
    );
  }

  void SsdFrame::snapshot(wlr_scene_tree* parent) const {
    if (!m_tree->node.enabled)
      return;
    auto* tree = parent;
    if (m_blur != nullptr && m_blur->node.enabled) {
      auto* copy = wlr_scene_blur_create(tree, m_blur->width, m_blur->height);
      if (copy != nullptr) {
        wlr_scene_node_lower_to_bottom(&copy->node);
        wlr_scene_blur_set_corner_radii(copy, m_blur->corners);
        wlr_scene_blur_set_alpha(copy, m_blur->alpha);
        wlr_scene_blur_set_should_only_blur_bottom_layer(copy, m_blur->should_only_blur_bottom_layer);
        wlr_scene_node_set_position(&copy->node, m_blur->node.x, m_blur->node.y);
      }
    }
    for (const auto* source : {m_background, m_separator, m_close, m_maximize, m_minimize}) {
      if (!source->node.enabled)
        continue;
      auto* copy = wlr_scene_rect_create(tree, source->width, source->height, source->color);
      copy->accepts_input = false;
      wlr_scene_rect_set_corner_radii(copy, source->corners);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    }
    if (m_label != nullptr && m_label->buffer != nullptr) {
      auto* copy = wlr_scene_buffer_create(tree, m_label->buffer);
      copy->point_accepts_input = rejectInput;
      wlr_scene_buffer_set_dest_size(copy, m_label->dst_width, m_label->dst_height);
      wlr_scene_buffer_set_opacity(copy, m_label->opacity);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + m_label->node.x, m_tree->node.y + m_label->node.y);
    }
  }
  std::unique_ptr<ViewChromeAttachment> SsdFrame::makePreview(wlr_scene_tree* parent) const {
    auto preview = std::make_unique<SsdFrame>(parent, m_labels);
    preview->m_preview = true;
    syncPreview(*preview);
    return preview;
  }
  void SsdFrame::syncPreview(ViewChromeAttachment& preview) const {
    if (auto* frame = dynamic_cast<SsdFrame*>(&preview)) {
      frame->setTitle(m_title);
      frame->setResizable(false);
      frame->setMaximizeEnabled(m_maximizeEnabled);
      frame->setButtons(m_buttons);
      frame->setMaximized(m_maximized);
      frame->setFocused(m_focused);
      frame->setBlurOptions(m_blurOptions);
      frame->reloadConfig();
    }
  }
} // namespace andromeda
