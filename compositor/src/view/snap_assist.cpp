#include "view/snap_assist.h"

#include "config/config.h"
#include "overview/app_icons.h"
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
    // Rulebook 4 grid: cards like the overview's Task View titles, an app icon over a one-line title.
    constexpr int kPad = 16;
    constexpr int kCardW = 168;
    constexpr int kCardH = 120;
    constexpr int kGap = 12;
    constexpr int kIcon = 48;
    constexpr int kSheetRadius = 16;
    constexpr int kCardRadius = 12;
    constexpr int kZoneSpacing = 8;

    std::array<float, 4> premultiplied(const std::array<float, 4>& color, float alpha) {
      const float a = color[3] * alpha;
      return {color[0] * a, color[1] * a, color[2] * a, a};
    }

    bool holds(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    std::string hex(const std::array<float, 4>& color) {
      const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      return std::format("#{:02x}{:02x}{:02x}", byte(color[0]), byte(color[1]), byte(color[2]));
    }

    bool zoneTaken(const Workspace& workspace, SnapZone zone) {
      return std::ranges::any_of(workspace.allViews(), [&](const View* view) {
        return view->mapped() && !view->minimized() && view->snapSpot() == std::optional<SnapSpot>(zone);
      });
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
    }
  }

  // The scene owns the tree and frees it with the rest of the chrome at teardown.
  SnapAssist::~SnapAssist() = default;

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
          m_cards.push_back({.view = Ref<View>(view)});
        }
      }
    }
    if (!target || m_cards.empty()) {
      return false;
    }
    m_zone = *target;
    m_box = snapZoneBox(anchor->floatingUsableArea(), m_zone, kZoneSpacing);
    m_lit = 0;
    m_open = true;
    wlr_scene_node_raise_to_top(&m_tree->node);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    build();
    return true;
  }

  void SnapAssist::build() {
    if (m_content != nullptr) {
      wlr_scene_node_destroy(&m_content->node);
    }
    m_content = wlr_scene_tree_create(m_tree);
    if (m_content == nullptr) {
      return;
    }
    const auto& colors = config().colors;
    m_sheet = wlr_scene_rect_create(m_content, m_box.width, m_box.height, premultiplied(colors.background, 0.92F).data());
    wlr_scene_rect_set_corner_radius(m_sheet, kSheetRadius);
    wlr_scene_node_set_position(&m_sheet->node, m_box.x, m_box.y);

    const wlr_output* output =
        wlr_output_layout_output_at(m_server.outputLayout(), m_box.x + m_box.width / 2.0, m_box.y + m_box.height / 2.0);
    const double scale = std::max(1.0, std::ceil(output != nullptr ? output->scale : 1.0));
    const int columns = std::max(1, (m_box.width - 2 * kPad + kGap) / (kCardW + kGap));
    const int rows = std::max(1, (m_box.height - 2 * kPad + kGap) / (kCardH + kGap));
    m_cards.resize(std::min(m_cards.size(), static_cast<size_t>(columns * rows)));
    const int used = std::min(columns, static_cast<int>(m_cards.size()));
    const int left = m_box.x + (m_box.width - (used * kCardW + (used - 1) * kGap)) / 2;
    const int top = m_box.y + kPad;
    for (size_t i = 0; i < m_cards.size(); ++i) {
      Card& card = m_cards[i];
      const int column = static_cast<int>(i) % columns;
      const int row = static_cast<int>(i) / columns;
      card.box = {left + column * (kCardW + kGap), top + row * (kCardH + kGap), kCardW, kCardH};
      card.rect = wlr_scene_rect_create(m_content, kCardW, kCardH, std::array<float, 4>{}.data());
      wlr_scene_rect_set_corner_radius(card.rect, kCardRadius);
      wlr_scene_node_set_position(&card.rect->node, card.box.x, card.box.y);
      View* view = card.view.get();
      if (view == nullptr) {
        continue;
      }
      if (const char* appId = view->appId(); appId != nullptr) {
        if (wlr_buffer* icon = m_icons->get(appId, static_cast<int>(kIcon * scale)); icon != nullptr) {
          wlr_scene_buffer* node = wlr_scene_buffer_create(m_content, icon);
          wlr_scene_buffer_set_dest_size(node, kIcon, kIcon);
          wlr_scene_node_set_position(&node->node, card.box.x + (kCardW - kIcon) / 2, card.box.y + 20);
        }
      }
      const char* title = view->title();
      const auto text = renderTextBuffer({
          .markup = std::format(
              "<span foreground=\"{}\">{}</span>", hex(colors.textPrimary), escapeMarkup(title != nullptr ? title : "")
          ),
          .font = "sans 10",
          .maxWidth = kCardW - 16,
          .padding = 0,
          .scale = scale,
          .ellipsize = true,
      });
      if (text.buffer != nullptr) {
        wlr_scene_buffer* node = wlr_scene_buffer_create(m_content, text.buffer);
        wlr_buffer_drop(text.buffer);
        wlr_scene_buffer_set_dest_size(node, text.logicalWidth, text.logicalHeight);
        wlr_scene_node_set_position(
            &node->node, card.box.x + (kCardW - text.logicalWidth) / 2, card.box.y + kCardH - 20 - text.logicalHeight / 2
        );
      }
    }
    draw();
  }

  void SnapAssist::draw() {
    const auto& colors = config().colors;
    for (size_t i = 0; i < m_cards.size(); ++i) {
      if (m_cards[i].rect != nullptr) {
        const auto color = i == m_lit ? premultiplied(colors.accentPrimary, 0.32F) : premultiplied(colors.textPrimary, 0.08F);
        wlr_scene_rect_set_color(m_cards[i].rect, color.data());
      }
    }
  }

  void SnapAssist::end() {
    m_open = false;
    m_cards.clear();
    if (m_content != nullptr) {
      wlr_scene_node_destroy(&m_content->node);
      m_content = nullptr;
      m_sheet = nullptr;
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
    View* view = index < m_cards.size() ? m_cards[index].view.get() : nullptr;
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
      if (holds(m_cards[i].box, lx, ly)) {
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
      draw();
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
    const size_t count = m_cards.size();
    const int columns = std::max(1, (m_box.width - 2 * kPad + kGap) / (kCardW + kGap));
    const auto move = [&](long by) {
      m_lit = static_cast<size_t>(((static_cast<long>(m_lit) + by) % static_cast<long>(count) + static_cast<long>(count))
                                  % static_cast<long>(count));
      draw();
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
      move(-columns);
      return true;
    case XKB_KEY_Down:
      move(columns);
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
