#include "view/snap_assist.h"

#include "config/config.h"
#include "design_tokens.h"
#include "output/output.h"
#include "overview/app_icons.h"
#include "overview/overview_internal.h"
#include "scene/glass_panel.h"
#include "scene/ssd_frame.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <xkbcommon/xkbcommon-keysyms.h>

namespace andromeda {
  namespace {
    // DESIGN.md "Design review fixes" R2, on the rulebook 4 grid: the zone outline sits 8 inside the scrim; cards are
    // thumbnails at window aspect, up to 3 columns 16 apart, each over an icon 16 and a footnote title.
    constexpr int kZoneSpacing = 8;
    constexpr int kOutlineInset = 8;
    constexpr int kOutlineRadius = 16;
    constexpr int kScrimRadius = kOutlineRadius + kOutlineInset;
    constexpr int kPad = 24;
    constexpr int kGap = 16;
    constexpr int kMaxColumns = 3;
    constexpr int kMaxRows = 3;
    constexpr int kMaxCardW = 280;
    constexpr int kMinCardW = 96;
    constexpr int kThumbRadius = 12;
    constexpr int kIcon = 16;
    constexpr int kIconGap = 6;
    constexpr int kLabelGap = 8;
    constexpr int kLabelH = 16;
    constexpr int kLensPad = 8;
    constexpr int kLensRadius = kThumbRadius + kLensPad;

    // A token (0xRRGGBBAA) as a premultiplied colour.
    std::array<float, 4> premultiplied(std::uint32_t token) {
      const auto channel = [token](int shift) { return static_cast<float>((token >> shift) & 0xFFU) / 255.0F; };
      const float a = channel(0);
      return {channel(24) * a, channel(16) * a, channel(8) * a, a};
    }

    bool holds(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    bool zoneTaken(const Workspace& workspace, SnapZone zone) {
      return std::ranges::any_of(workspace.allViews(), [&](const View* view) {
        return view->mapped() && !view->minimized() && view->snapSpot() == std::optional<SnapSpot>(zone);
      });
    }

    const Tokens::Scheme& scheme() {
      return config().appearance.scheme == ColorScheme::Light ? Tokens::schemeLight : Tokens::schemeDark;
    }

    int titleBar(const View& view) { return view.ssdFrame() != nullptr ? view.titleBarHeight() : 0; }

    // Window height over width, title bar included.
    double aspect(const View& view) {
      const wlr_box geometry = view.geometryBox();
      if (geometry.width <= 0 || geometry.height <= 0) {
        return 0.75;
      }
      return static_cast<double>(geometry.height + titleBar(view)) / geometry.width;
    }

    struct SurfaceWalk {
      View* view;
      wlr_scene_tree* parent;
      wlr_box geometry;
      double zoom;
      int radius;
      bool roundTop;
    };

    void copySurface(wlr_surface* surface, int sx, int sy, void* data) {
      const auto* walk = static_cast<const SurfaceWalk*>(data);
      wlr_scene_buffer* source = ovimpl::sourceBufferForSurface(&walk->view->sceneTree()->node, surface);
      if (source == nullptr || source->buffer == nullptr || !source->node.enabled) {
        return;
      }
      wlr_scene_buffer* copy = wlr_scene_buffer_create(walk->parent, source->buffer);
      if (copy == nullptr) {
        return;
      }
      int width = 0;
      int height = 0;
      ovimpl::viewSurfaceSize(source, surface, &width, &height);
      const auto scaled = [zoom = walk->zoom](int v) { return static_cast<int>(std::lround(v * zoom)); };
      wlr_scene_node_set_position(&copy->node, scaled(sx - walk->geometry.x), scaled(sy - walk->geometry.y));
      wlr_scene_buffer_set_dest_size(copy, std::max(1, scaled(width)), std::max(1, scaled(height)));
      if (source->src_box.width > 0 && source->src_box.height > 0) {
        wlr_scene_buffer_set_source_box(copy, &source->src_box);
      }
      wlr_scene_buffer_set_transform(copy, source->transform);
      wlr_scene_buffer_copy_color(copy, source);
      wlr_scene_buffer_set_filter_mode(copy, WLR_SCALE_FILTER_BILINEAR);
      copy->point_accepts_input = ovimpl::rejectInput;
      if (surface == walk->view->rootSurface()) {
        const int top = walk->roundTop ? walk->radius : 0;
        wlr_scene_buffer_set_corner_radii(copy, corner_radii_new(top, top, walk->radius, walk->radius));
        const wlr_box corners{scaled(walk->geometry.x - sx), scaled(walk->geometry.y - sy),
                              scaled(walk->geometry.width), scaled(walk->geometry.height)};
        wlr_scene_buffer_set_corner_box(copy, &corners);
      }
    }
  } // namespace

  SnapAssist::SnapAssist(Server& server, wlr_scene_tree* parent)
      : m_server(server), m_tree(wlr_scene_tree_create(parent)),
        m_icons(std::make_unique<ov::AppIcons>(wl_display_get_event_loop(server.display()), [this] {
          if (m_open) {
            build();
          }
        })) {
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
      m_scrim = std::make_unique<GlassPanel>(m_tree, glass::Kind::Sheet);
      const float clear[4]{};
      m_outline = wlr_scene_rect_create(m_tree, 1, 1, clear);
      m_lensShadow = wlr_scene_shadow_create(m_tree, 1, 1, 0, 1.0F, clear);
      m_lens = wlr_scene_rect_create(m_tree, 1, 1, clear);
    }
    m_server.registerAnimatable(this);
  }

  // The scene owns the tree and frees it with the rest of the chrome at teardown.
  SnapAssist::~SnapAssist() { m_server.unregisterAnimatable(this); }

  void SnapAssist::start(View& anchor, SnapZone zone) {
    end();
    m_members = {{Ref<View>(&anchor), zone}};
    m_zone = zone;
    if (!showNextZone()) {
      m_members.clear();
    }
  }

  bool SnapAssist::showNextZone() {
    View* anchor = m_members.empty() ? nullptr : m_members.front().view.get();
    Workspace* workspace = anchor != nullptr ? anchor->workspace() : nullptr;
    if (m_tree == nullptr || workspace == nullptr) {
      return false;
    }
    const size_t layout = m_members.front().zone.layout;
    std::optional<SnapZone> target;
    for (size_t zone = 0; zone < snapLayoutZones(layout) && !target; ++zone) {
      const SnapZone candidate{.layout = static_cast<uint8_t>(layout), .zone = static_cast<uint8_t>(zone)};
      if (!zoneTaken(*workspace, candidate)) {
        target = candidate;
      }
    }
    m_cards.clear();
    if (target) {
      for (View* view : std::views::reverse(workspace->allViews())) {
        const auto spot = view->snapSpot();
        const auto* zone = spot ? std::get_if<SnapZone>(&*spot) : nullptr;
        if (view->mapped() && view->floating() && !view->minimized() && view->canSnap()
            && (zone == nullptr || zone->layout != layout)) {
          auto card = std::make_unique<Card>();
          card->view = Ref<View>(view);
          m_cards.push_back(std::move(card));
        }
      }
    }
    if (!target || m_cards.empty()) {
      return false;
    }
    m_zone = *target;
    m_box = snapZoneBox(anchor->floatingUsableArea(), m_zone, kZoneSpacing);
    m_output = wlr_output_layout_output_at(
        m_server.outputLayout(), m_box.x + m_box.width / 2.0, m_box.y + m_box.height / 2.0
    );
    m_lit = 0;
    m_open = true;
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    build();
    moveLens(false);
    return true;
  }

  void SnapAssist::build() {
    if (m_content != nullptr) {
      wlr_scene_node_destroy(&m_content->node);
    }
    m_content = wlr_scene_tree_create(m_tree);
    if (m_content == nullptr || m_outline == nullptr || m_lens == nullptr || m_lensShadow == nullptr) {
      return;
    }
    const auto& colors = scheme();
    const bool light = config().appearance.scheme == ColorScheme::Light;

    // Rulebook scrim over the empty zone only, on a plain blur of what it covers; the outline marks the zone's edge.
    m_scrim->setScrim(premultiplied(colors.scrim));
    m_scrim->place(m_box, kScrimRadius);
    const wlr_box outline{m_box.x + kOutlineInset, m_box.y + kOutlineInset, m_box.width - 2 * kOutlineInset,
                          m_box.height - 2 * kOutlineInset};
    wlr_scene_node_set_position(&m_outline->node, outline.x, outline.y);
    wlr_scene_rect_set_size(m_outline, outline.width, outline.height);
    wlr_scene_rect_set_corner_radius(m_outline, kOutlineRadius);
    // The scrim always darkens, so the outline takes the dark scheme's label_3 (light grey) in both schemes.
    wlr_scene_rect_set_color(m_outline, premultiplied(Tokens::schemeDark.label3).data());
    wlr_scene_rect_set_clipped_region(
        m_outline,
        clipped_region{
            .area = {1, 1, std::max(0, outline.width - 2), std::max(0, outline.height - 2)},
            .corners = corner_radii_new(
                kOutlineRadius - 1, kOutlineRadius - 1, kOutlineRadius - 1, kOutlineRadius - 1
            ),
        }
    );

    // Grid: as many cards as fit at kMinCardW or more, up to 3 x 3; slots are 4:3, each thumbnail fits its slot.
    const int areaW = outline.width - 2 * kPad;
    const int areaH = outline.height - 2 * kPad;
    const int rowExtra = kLabelGap + kLabelH;
    size_t count = std::min(m_cards.size(), static_cast<size_t>(kMaxColumns * kMaxRows));
    int columns = 1;
    int rows = 1;
    int cardW = 0;
    for (;; --count) {
      columns = std::min(kMaxColumns, static_cast<int>(count));
      rows = (static_cast<int>(count) + columns - 1) / columns;
      const int byWidth = (areaW - (columns - 1) * kGap) / columns;
      const int byHeight = ((areaH - (rows - 1) * kGap) / rows - rowExtra) * 4 / 3;
      cardW = std::min({kMaxCardW, byWidth, byHeight});
      if (cardW >= kMinCardW || count == 1) {
        break;
      }
    }
    cardW = std::max(cardW, 1);
    m_cards.resize(count);
    m_columns = columns;
    const int slotH = cardW * 3 / 4;
    const int cardH = slotH + rowExtra;
    const int left = outline.x + (outline.width - (columns * cardW + (columns - 1) * kGap)) / 2;
    const int top = outline.y + (outline.height - (rows * cardH + (rows - 1) * kGap)) / 2;
    const double scale = std::max(1.0, std::ceil(m_output != nullptr ? m_output->scale : 1.0));

    for (size_t i = 0; i < m_cards.size(); ++i) {
      Card& card = *m_cards[i];
      View* view = card.view.get();
      const int column = static_cast<int>(i) % columns;
      const int row = static_cast<int>(i) / columns;
      card.box = {left + column * (cardW + kGap), top + row * (cardH + kGap), cardW, cardH};
      card.frame.reset();
      card.commit.disconnect();
      card.destroy.disconnect();
      card.tree = nullptr;
      card.surfaces = nullptr;
      if (view == nullptr) {
        continue;
      }
      const double ratio = aspect(*view);
      const int thumbW = ratio * cardW > slotH ? static_cast<int>(std::lround(slotH / ratio)) : cardW;
      const int thumbH = std::min(slotH, static_cast<int>(std::lround(cardW * ratio)));
      card.thumb = {card.box.x + (cardW - thumbW) / 2, card.box.y + (slotH - thumbH) / 2, thumbW, thumbH};
      const wlr_box geometry = view->geometryBox();
      card.zoom = geometry.width > 0 ? static_cast<double>(thumbW) / geometry.width : 1.0;
      card.tree = wlr_scene_tree_create(m_content);
      if (card.tree != nullptr) {
        wlr_scene_node_set_position(&card.tree->node, card.thumb.x, card.thumb.y);
        const wlr_box clip{0, 0, thumbW, thumbH};
        wlr_scene_tree_set_clip(card.tree, &clip);
        syncThumbnail(card);
        if (wlr_surface* root = view->rootSurface(); root != nullptr) {
          card.commit.connect(&root->events.commit, [this, &card](void*) { syncThumbnail(card); });
          card.destroy.connect(&root->events.destroy, [&card](void*) {
            card.commit.disconnect();
            card.destroy.disconnect();
          });
        }
      }

      // Label: icon 16 and the title (footnote 500, label_1), centred under the slot as one group.
      const char* title = view->title();
      const auto text = renderTextBuffer({
          .markup = std::format(
              "<span weight=\"500\" foreground=\"#{:06x}\" alpha=\"{}%\">{}</span>", colors.label1 >> 8,
              static_cast<int>(std::lround((colors.label1 & 0xFFU) * 100.0 / 255.0)),
              escapeMarkup(title != nullptr ? title : "")
          ),
          .font = "sans 9",
          .maxWidth = cardW - kIcon - kIconGap,
          .padding = 0,
          .scale = scale,
          .ellipsize = true,
      });
      wlr_buffer* icon = nullptr;
      if (const char* appId = view->appId(); appId != nullptr) {
        icon = m_icons->get(appId, static_cast<int>(kIcon * scale));
      }
      const int textW = text.buffer != nullptr ? text.logicalWidth : 0;
      const int groupW = (icon != nullptr ? kIcon + kIconGap : 0) + textW;
      int x = card.box.x + (cardW - groupW) / 2;
      const int labelY = card.box.y + slotH + kLabelGap;
      if (icon != nullptr) {
        wlr_scene_buffer* node = wlr_scene_buffer_create(m_content, icon);
        wlr_scene_buffer_set_dest_size(node, kIcon, kIcon);
        wlr_scene_node_set_position(&node->node, x, labelY + (kLabelH - kIcon) / 2);
        x += kIcon + kIconGap;
      }
      if (text.buffer != nullptr) {
        wlr_scene_buffer* node = wlr_scene_buffer_create(m_content, text.buffer);
        wlr_buffer_drop(text.buffer);
        wlr_scene_buffer_set_dest_size(node, text.logicalWidth, text.logicalHeight);
        wlr_scene_node_set_position(&node->node, x, labelY + (kLabelH - text.logicalHeight) / 2);
      }
    }

    // Rulebook 6 lens behind the current card: white 92 % light / 14 % dark with E1.
    wlr_scene_rect_set_color(m_lens, premultiplied(colors.lens).data());
    wlr_scene_rect_set_corner_radius(m_lens, kLensRadius);
    const auto& e1 = Tokens::elevationE1;
    const auto shadow = premultiplied((colors.shadow & ~0xFFU) | static_cast<std::uint32_t>(std::lround(
                                          (light ? e1.keyLight : e1.keyDark) * 255.0F
                                      )));
    wlr_scene_shadow_set_color(m_lensShadow, shadow.data());
    if (m_lit >= m_cards.size()) {
      m_lit = 0;
    }
    placeLens();
  }

  void SnapAssist::syncThumbnail(Card& card) {
    View* view = card.view.get();
    if (view == nullptr || card.tree == nullptr) {
      return;
    }
    const int bar = static_cast<int>(std::lround(titleBar(*view) * card.zoom));
    if (card.surfaces != nullptr) {
      wlr_scene_node_destroy(&card.surfaces->node);
    }
    card.surfaces = wlr_scene_tree_create(card.tree);
    if (card.surfaces == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&card.surfaces->node, 0, bar);
    if (wlr_surface* root = view->rootSurface(); root != nullptr) {
      SurfaceWalk walk{
          .view = view,
          .parent = card.surfaces,
          .geometry = view->geometryBox(),
          .zoom = card.zoom,
          .radius = kThumbRadius,
          .roundTop = bar == 0,
      };
      wlr_surface_for_each_surface(root, copySurface, &walk);
    }
    if (const SsdFrame* live = view->ssdFrame(); live != nullptr) {
      if (card.frame == nullptr) {
        card.frame = live->makePreview(card.tree);
      }
      if (card.frame != nullptr) {
        const wlr_box geometry = view->geometryBox();
        live->syncPreview(*card.frame);
        card.frame->setAlpha(1.0F);
        card.frame->layout({
            .contentX = 0,
            .contentY = static_cast<int>(std::lround(bar / card.zoom)),
            .contentWidth = geometry.width,
            .contentHeight = geometry.height,
            .cornerRadius = static_cast<int>(std::lround(kThumbRadius / card.zoom)),
            .titleBarHeight = titleBar(*view),
            .scale = m_output != nullptr ? m_output->scale : 1.0F,
            .zoom = card.zoom,
        });
      }
    }
  }

  void SnapAssist::moveLens(bool animate) {
    if (m_lit >= m_cards.size()) {
      return;
    }
    const wlr_box& box = m_cards[m_lit]->box;
    const std::array<double, 4> target{static_cast<double>(box.x - kLensPad), static_cast<double>(box.y - kLensPad),
                                       static_cast<double>(box.width + 2 * kLensPad),
                                       static_cast<double>(box.height + 2 * kLensPad)};
    const auto& anim = config().animation;
    // Rulebook 7.6 P1: the lens travels on the snap spring, never fades between cards.
    const SpringConfig spring{.damping = Tokens::springSnap.damping, .stiffness = Tokens::springSnap.stiffness};
    for (size_t i = 0; i < target.size(); ++i) {
      if (animate && anim.enabled) {
        m_lensBox[i].settleSpring(target[i], spring);
      } else {
        m_lensBox[i].snap(target[i]);
      }
    }
    placeLens();
    if (animate && anim.enabled && m_output != nullptr) {
      wlr_output_schedule_frame(m_output);
    }
  }

  void SnapAssist::placeLens() {
    if (m_lens == nullptr || m_lensShadow == nullptr) {
      return;
    }
    const bool show = m_open && m_lit < m_cards.size();
    wlr_scene_node_set_enabled(&m_lens->node, show);
    wlr_scene_node_set_enabled(&m_lensShadow->node, show);
    if (!show) {
      return;
    }
    const int x = static_cast<int>(std::lround(m_lensBox[0].current()));
    const int y = static_cast<int>(std::lround(m_lensBox[1].current()));
    const int w = std::max(1, static_cast<int>(std::lround(m_lensBox[2].current())));
    const int h = std::max(1, static_cast<int>(std::lround(m_lensBox[3].current())));
    wlr_scene_node_set_position(&m_lens->node, x, y);
    wlr_scene_rect_set_size(m_lens, w, h);
    const auto& e1 = Tokens::elevationE1;
    const int sigma = std::max(1, static_cast<int>(std::lround(e1.keyBlur / 2.0F)));
    const int offsetY = static_cast<int>(std::lround(e1.keyY));
    wlr_scene_node_set_position(&m_lensShadow->node, x - sigma, y - sigma + offsetY);
    wlr_scene_shadow_set_size(m_lensShadow, w + 2 * sigma, h + 2 * sigma);
    wlr_scene_shadow_set_blur_sigma(m_lensShadow, static_cast<float>(sigma));
    wlr_scene_shadow_set_corner_radius(m_lensShadow, kLensRadius);
  }

  bool SnapAssist::tickAnimations(double nowMsec) {
    bool running = false;
    for (auto& value : m_lensBox) {
      running = value.tick(nowMsec) || running;
    }
    placeLens();
    return running;
  }

  bool SnapAssist::hasActiveAnimations() const {
    return std::ranges::any_of(m_lensBox, [](const AnimatedValue& value) { return value.animating(); });
  }

  bool SnapAssist::animatesOn(const Output* output) const {
    return output != nullptr && output->wlr() == m_output && hasActiveAnimations();
  }

  void SnapAssist::end() {
    m_open = false;
    m_cards.clear();
    if (m_content != nullptr) {
      wlr_scene_node_destroy(&m_content->node);
      m_content = nullptr;
    }
    for (auto& value : m_lensBox) {
      value.snap(value.target());
    }
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
  }

  void SnapAssist::finish() {
    end();
    std::erase_if(m_members, [](const Member& member) { return member.view.get() == nullptr; });
    if (m_members.size() >= 2) {
      // A window belongs to one group: picking it into a new one takes it out of the old.
      for (auto& group : m_groups) {
        std::erase_if(group, [&](const Member& old) {
          return std::ranges::any_of(m_members, [&](const Member& m) { return m.view.get() == old.view.get(); });
        });
      }
      std::erase_if(m_groups, [](const auto& group) { return group.size() < 2; });
      m_groups.push_back(std::move(m_members));
    }
    m_members.clear();
  }

  void SnapAssist::pick(size_t index) {
    View* view = index < m_cards.size() ? m_cards[index]->view.get() : nullptr;
    if (view == nullptr || !view->snapTo(SnapSpot{m_zone})) {
      finish();
      return;
    }
    m_members.push_back({Ref<View>(view), m_zone});
    if (!showNextZone()) {
      finish();
    }
  }

  std::optional<size_t> SnapAssist::cardAt(double lx, double ly) const {
    for (size_t i = 0; i < m_cards.size(); ++i) {
      if (holds(m_cards[i]->box, lx, ly)) {
        return i;
      }
    }
    return std::nullopt;
  }

  bool SnapAssist::anchorHolds() {
    const View* anchor = m_members.empty() ? nullptr : m_members.front().view.get();
    if (anchor != nullptr && anchor->mapped() && anchor->snapSpot() == std::optional<SnapSpot>(m_members.front().zone)) {
      return true;
    }
    finish();
    return false;
  }

  void SnapAssist::hover(double lx, double ly) {
    if (m_open && !anchorHolds()) {
      return;
    }
    if (const auto index = m_open ? cardAt(lx, ly) : std::nullopt; index && *index != m_lit) {
      m_lit = *index;
      moveLens(true);
    }
  }

  bool SnapAssist::press(double lx, double ly) {
    if (!m_open || !anchorHolds()) {
      return false;
    }
    if (const auto index = cardAt(lx, ly)) {
      pick(*index);
      return true;
    }
    if (holds(m_box, lx, ly)) {
      return true;
    }
    finish();
    return false;
  }

  bool SnapAssist::key(uint32_t keysym) {
    if (!m_open || m_cards.empty() || !anchorHolds()) {
      return false;
    }
    const auto count = static_cast<long>(m_cards.size());
    const auto move = [&](long by) {
      m_lit = static_cast<size_t>(((static_cast<long>(m_lit) + by) % count + count) % count);
      moveLens(true);
    };
    switch (keysym) {
    case XKB_KEY_Left:
      move(-1);
      return true;
    case XKB_KEY_Right:
    case XKB_KEY_Tab:
      move(1);
      return true;
    case XKB_KEY_Up:
      move(-m_columns);
      return true;
    case XKB_KEY_Down:
      move(m_columns);
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
      pick(m_lit);
      return true;
    case XKB_KEY_Escape:
      finish();
      return true;
    default:
      finish();
      return false;
    }
  }

  std::vector<View*> SnapAssist::groupMates(const View* view) {
    if (!config().input.snapGroups) {
      return {};
    }
    // Members still in their zone and on the first member's workspace stay; the rest drop out.
    for (auto& group : m_groups) {
      const Workspace* home = group.empty() || group.front().view.get() == nullptr ? nullptr
                                                                                   : group.front().view->workspace();
      std::erase_if(group, [&](const Member& member) {
        const View* v = member.view.get();
        return v == nullptr || !v->mapped() || v->workspace() != home
            || v->snapSpot() != std::optional<SnapSpot>(member.zone);
      });
    }
    std::erase_if(m_groups, [](const auto& group) { return group.size() < 2; });
    for (const auto& group : m_groups) {
      if (std::ranges::any_of(group, [&](const Member& member) { return member.view.get() == view; })) {
        std::vector<View*> mates;
        for (const Member& member : group) {
          if (member.view.get() != view) {
            mates.push_back(member.view.get());
          }
        }
        return mates;
      }
    }
    return {};
  }
} // namespace andromeda
