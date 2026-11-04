#pragma once

#include "render/scene/accessible.h"

// Launcher helpers and row nodes shared by the launcher_panel*.cpp files.

#include "shell/launcher/launcher_panel.h"
#include "config/app_icon_settings.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "launcher/app_provider.h"
#include "launcher/clipboard_provider.h"
#include "launcher/files_provider.h"
#include "launcher/klipy_provider.h"
#include "render/core/image_decoder.h"
#include "render/core/texture_manager.h"
#include "util/file_utils.h"
#include "launcher/search_ranker.h"
#include "render/animation/animation_manager.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/pinned_apps.h"
#include "core/log.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "shell/profile/avatar_path.h"
#include "system/desktop_entry.h"
#include "time/time_format.h"
#include "system/distro_info.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_list_view.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace launcher_detail {

  constexpr Logger kLog("launcher");

  // The first visible node of type T in a launcher cell, depth first.
  template <typename T> const Node* firstVisible(const Node& node) {
    if (!node.visible()) {
      return nullptr;
    }
    if (dynamic_cast<const T*>(&node) != nullptr) {
      return &node;
    }
    for (const auto& child : node.children()) {
      if (const Node* found = firstVisible<T>(*child); found != nullptr) {
        return found;
      }
    }
    return nullptr;
  }

  // The cell's icon, an image or else a symbol: the pixels an app launch starts from.
  inline const Node* cellIcon(const Node& cell) {
    const Node* image = firstVisible<Image>(cell);
    return image != nullptr ? image : firstVisible<Glyph>(cell);
  }

  constexpr std::size_t kRowOverscan = 3;
  // Minimum trimmed query length before prefixed opt-in providers join the global search.
  constexpr float kIconSizeDefault = 40.0F;
  constexpr float kIconSizeCompact = 28.0F;
  constexpr std::size_t kAppGridColumns = 6;
  // Emoji mode grid (DESIGN.md "Launcher media modes"): 8 cells across the 360 list pane.
  constexpr std::size_t kEmojiColumns = 8;
  constexpr std::size_t kMaxSectionColumns = kEmojiColumns;
  // Klipy "sm" GIFs are short loops; the caps bound GPU memory for a hostile file.
  constexpr int kMaxPreviewGifFrames = 120;
  constexpr std::size_t kMaxPreviewGifBytes = 64ULL * 1024 * 1024;

  // Cell grids of the media modes: emoji glyphs, or GIF and sticker thumbnails.
  enum class CellGrid : std::uint8_t { None, Emoji, Gif, Sticker };

  [[nodiscard]] inline std::size_t cellColumns(CellGrid grid) {
    switch (grid) {
    case CellGrid::Emoji:
      return kEmojiColumns;
    case CellGrid::Gif:
      return 3;
    case CellGrid::Sticker:
      return 4;
    case CellGrid::None:
      break;
    }
    return 0;
  }

  [[nodiscard]] inline float cellRowHeight(CellGrid grid) {
    switch (grid) {
    case CellGrid::Gif:
      return Tokens::componentLauncherGifCell;
    case CellGrid::Sticker:
      return Tokens::componentLauncherStickerCell;
    case CellGrid::Emoji:
    case CellGrid::None:
      break;
    }
    return Tokens::componentLauncherRow;
  }
  // Rulebook 9.14 at its tallest (the apps grid): field, hairline, p, grid rows with g 8, page dots, p.
  [[nodiscard]] inline float launcherSheetHeight() {
    const float rows = static_cast<float>(Tokens::componentLauncherRows);
    return Tokens::componentLauncherSearch + 1.0F + 2.0F * Tokens::componentLauncherPadding
        + rows * Tokens::componentLauncherCellHeight + rows * Tokens::spaceP8 + Tokens::componentPageDotsRow;
  }
  [[nodiscard]] inline bool isAppsQuery(std::string_view query) { return query == "apps"; }
  [[nodiscard]] inline bool isTwoLineRow(const LauncherResult& result) { return result.presentation.starts_with("clipboard"); }
  [[nodiscard]] inline std::string formatByteSize(std::size_t bytes) {
    if (bytes < 1024) {
      return std::format("{} B", bytes);
    }
    if (bytes < 1024 * 1024) {
      return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  // Tab order of the media modes; a mode shows only when its provider is registered.
  constexpr std::array<std::string_view, 5> kModeProviders{"Clipboard", "Emoji", "Gifs", "Stickers", "Symbols"};
  // Files scope kind chips: the daemon's kind word ("" = all) and the label key under launcher.files.kinds.
  // Sticker packs: provider pack word, translation key.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kStickerPacks{{
      {"", "all"}, {"klipy", "klipy"}, {"fluent", "fluent"},
  }};
  constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kFileKinds{{
      {"", "all"}, {"folder", "folder"}, {"pdf", "pdf"}, {"image", "image"}, {"video", "video"},
      {"audio", "audio"}, {"doc", "doc"}, {"sheet", "sheet"}, {"slides", "slides"},
  }};
  constexpr std::string_view kModeContextPrefix = "mode:";
  constexpr std::int32_t kCompactNavigationWidth = 680;
  constexpr float kLauncherAvatarSize = 44.0F;
  constexpr std::string_view kApplicationsProviderId = "Applications";
  constexpr std::string_view kSessionProviderId = "Session";
  constexpr std::string_view kProviderOverviewProviderId = "__launcher_provider_overview__";
  constexpr std::string_view kProviderOverviewResultPrefix = "provider:";

  [[nodiscard]] inline bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] inline std::string singleLinePreview(std::string_view text) {
    std::string preview;
    preview.reserve(text.size());
    bool lastWasSpace = false;
    for (const char c : text) {
      const bool whitespace = c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
      if (whitespace) {
        if (!lastWasSpace) {
          preview.push_back(' ');
          lastWasSpace = true;
        }
        continue;
      }
      preview.push_back(c);
      lastWasSpace = c == ' ';
    }
    return preview;
  }

  [[nodiscard]] inline bool isDetailPresentation(const LauncherResult& result) { return result.presentation == "detail"; }
  [[nodiscard]] inline bool isMediaPresentation(const LauncherResult& result) {
    return result.presentation == "gif" || result.presentation == "sticker" || result.presentation == "file-image";
  }

  [[nodiscard]] inline std::string providerOverviewId(std::string_view prefix) {
    std::string id(kProviderOverviewResultPrefix);
    id += prefix;
    return id;
  }

  struct LauncherListStyle {
    float scale = 1.0F;
    bool showIcons = true;
    bool showAppOriginIndicator = true;
    bool compact = false;
    // Rulebook 9.14 pill rows: icon 24, one line, lens selection.
    bool pill = false;
    std::optional<ColorSpec> appIconColorizeTint;
    std::optional<ColorSpec> listItemBackground;
  };

  [[nodiscard]] inline float launcherIconSize(const LauncherListStyle& style) {
    if (style.pill) {
      return Tokens::componentLauncherRowIcon * style.scale;
    }
    return (style.compact ? kIconSizeCompact : kIconSizeDefault) * style.scale;
  }

  [[nodiscard]] inline float rowGap(const LauncherListStyle& style) {
    if (style.pill) {
      return Tokens::spaceP12 * style.scale;
    }
    return (style.compact ? Style::spaceSm : Style::spaceMd) * style.scale;
  }

  [[nodiscard]] inline float launcherGridIconSize(const LauncherListStyle& style) { return Tokens::componentLauncherIcon * style.scale; }

  [[nodiscard]] inline float stableLabelHeight(const TextMetrics& metrics) { return std::round(metrics.bottom - metrics.top); }

  [[nodiscard]] inline float launcherTextStackHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float bodySize = Style::fontSizeBody * style.scale;
    float textHeight = stableLabelHeight(renderer.measureFont(bodySize, {FontWeight::SemiBold}));
    if (!style.compact) {
      const float captionSize = Style::fontSizeCaption * style.scale;
      textHeight += stableLabelHeight(renderer.measureFont(captionSize, {FontWeight::Normal}));
    }
    return textHeight;
  }

  [[nodiscard]] inline float launcherRowHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float textHeight = launcherTextStackHeight(renderer, style);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] inline float launcherRowHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float bodySize = Style::fontSizeBody * style.scale;
    const float captionSize = Style::fontSizeCaption * style.scale;
    const float textHeight = bodySize + (style.compact ? 0.0F : captionSize);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] inline LauncherListStyle
  launcherListStyleFrom(bool pill, const ConfigService* config, float scale, float cardOpacity) {
    LauncherListStyle style{
        .scale = scale, .pill = pill, .appIconColorizeTint = std::nullopt, .listItemBackground = std::nullopt
    };
    if (config != nullptr) {
      const auto& launcher = config->config().shell.launcher;
      style.showIcons = launcher.showIcons;
      style.showAppOriginIndicator = launcher.showAppOriginIndicator;
      style.compact = launcher.compact;
      style.appIconColorizeTint = effectiveShellAppIconColorizationTint(config->config().shell);
      if (config->config().shell.panel.listItemBackground) {
        style.listItemBackground = colorSpecFromRole(ColorRole::Fill2, cardOpacity);
      }
    }
    return style;
  }

  class LauncherResultRow final : public Node {
  public:
    LauncherResultRow(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float iconSize = launcherIconSize(m_style);
      const float gap = rowGap(m_style);
      const float paddingV =
          m_style.pill ? 0.0F : (m_style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * m_style.scale;
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = gap,
           .paddingV = paddingV,
           .paddingH = (m_style.pill ? Tokens::spaceP12 : Style::spaceSm) * m_style.scale,
           .radius = m_style.pill ? Tokens::radiusR12 * m_style.scale : Style::scaledRadiusMd(m_style.scale)}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::label({
              .out = &m_badgeLabel,
              .fontSize = iconSize,
              .color = colorSpecFromRole(ColorRole::Label1),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::image({
              .out = &m_image,
              .width = iconSize,
              .height = iconSize,
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = iconSize,
              .color = colorSpecFromRole(ColorRole::Label1),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this] {
        if (!m_style.showIcons
            || m_badgeVisible
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textCol,
                  .align = FlexAlign::Start,
                  .gap = 0.0F,
                  .flexGrow = 1.0F,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * m_style.scale,
                  .fontWeight = m_style.pill ? FontWeight::Normal : FontWeight::SemiBold,
                  .color = colorSpecFromRole(ColorRole::Label1),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              }),
              ui::label({
                  .out = &m_subtitle,
                  .fontSize = Style::fontSizeCaption * m_style.scale,
                  .color = colorSpecFromRole(ColorRole::Label2),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              })
          )
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::label({
              .out = &m_openHint,
              .text = i18n::tr("launcher.pill.open-hint"),
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .maxLines = 1,
              .visible = false,
              .participatesInLayout = false,
          })
      );
      if (m_style.pill) {
        // Title, then the kind after g 8 on the same line.
        m_textCol->setDirection(FlexDirection::Horizontal);
        m_textCol->setAlign(FlexAlign::Center);
        m_textCol->setGap(Tokens::spaceP8 * m_style.scale);
      }
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    // A clipboard image thumbnail is a 32 square cover crop (launcher-media.html); other rows show a 24 icon.
    [[nodiscard]] float leadingSize() const {
      return m_thumbnail ? Tokens::spaceP32 * m_style.scale : launcherIconSize(m_style);
    }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      auto& accessible = accessibleState();
      accessible.role = AccessibleRole::ListItem;
      accessible.name = result.title;
      accessible.description = result.subtitle;
      accessible.selected = selected;
      accessible.actionName = "activate";
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      m_thumbnail = result.presentation == "clipboard-image";
      m_image->setFit(m_thumbnail ? ImageFit::Cover : ImageFit::Contain);
      m_image->setRadius(m_thumbnail ? Tokens::radiusR8 * m_style.scale : 0.0F);
      const float iconSize = leadingSize();
      m_iconTargetSize = static_cast<int>(std::round(iconSize));
      m_badgeVisible = !result.badge.empty();
      m_rowHeight = height;

      setSize(width, height);
      m_row->setFrameSize(width, height);

      m_badgeLabel->setVisible(false);
      m_badgeLabel->setParticipatesInLayout(false);
      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      const bool showAppIcon = m_style.showIcons && !m_badgeVisible;
      const bool showLeadingVisual = m_badgeVisible || showAppIcon;
      if (m_badgeVisible) {
        m_badgeLabel->setText(singleLinePreview(result.badge));
        m_badgeLabel->setSize(iconSize, iconSize);
        m_badgeLabel->setVisible(true);
        m_badgeLabel->setParticipatesInLayout(true);
        m_image->clear(renderer);
      } else if (showAppIcon) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float gap = rowGap(m_style);
      const float horizontalPad = (m_style.pill ? Tokens::spaceP12 : Style::spaceSm) * m_style.scale * 2.0F;
      const bool showOpenHint = false; // direction C: the preview pane holds Open
      m_openHint->setVisible(showOpenHint);
      m_openHint->setParticipatesInLayout(showOpenHint);
      const float leadingWidth = showLeadingVisual ? iconSize + gap : 0.0F;
      const float pinnedWidth = result.pinned ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      m_pinnedGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setParticipatesInLayout(result.pinned);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setParticipatesInLayout(hasOrigin);
      const float originWidth = hasOrigin ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      const float hintWidth = showOpenHint ? m_openHint->width() + gap : 0.0F;
      const float textWidth =
          std::max(0.0F, width - leadingWidth - pinnedWidth - originWidth - hintWidth - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      const bool showSubtitle = !m_style.compact && (!m_style.pill || result.presentation.starts_with("clipboard") || result.presentation == "symbol-wide")
          && !result.subtitle.empty();
      // Clipboard rows put the subtitle under the title (row_tall); other pill rows put it after the title.
      const bool twoLine = m_style.pill && isTwoLineRow(result);
      if (m_style.pill) {
        m_textCol->setDirection(twoLine ? FlexDirection::Vertical : FlexDirection::Horizontal);
        m_textCol->setAlign(twoLine ? FlexAlign::Start : FlexAlign::Center);
        m_textCol->setGap(twoLine ? 0.0F : Tokens::spaceP8 * m_style.scale);
      }
      if (!showSubtitle) {
        m_subtitle->setVisible(false);
        m_subtitle->setText("");
      } else {
        m_subtitle->setVisible(true);
        m_subtitle->setText(singleLinePreview(result.subtitle));
        // A pill row puts the kind after the title on one line; the title keeps its width first.
        m_subtitle->setMaxWidth(
            m_style.pill && !twoLine ? std::max(0.0F, textWidth - m_title->width() - Tokens::spaceP8 * m_style.scale)
                                     : textWidth
        );
      }

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_badgeVisible || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      m_image->setSize(leadingSize(), leadingSize());
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_style.showIcons && !m_badgeVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_style.pill) {
        // Rulebook 9.14: the current row is the lens; text keeps its label colours.
        m_row->setFill(
            m_selected ? colorSpecFromRole(ColorRole::Lens)
                       : (m_hovered ? colorSpecFromRole(ColorRole::Fill1) : clearColorSpec())
        );
        m_row->clearBorder();
        const ColorSpec label1 = colorSpecFromRole(ColorRole::Label1);
        const ColorSpec label2 = colorSpecFromRole(ColorRole::Label2);
        m_badgeLabel->setColor(label1);
        m_glyph->setColor(label1);
        m_title->setColor(label1);
        m_subtitle->setColor(label2);
        m_pinnedGlyph->setColor(label2);
        m_originGlyph->setColor(label2);
        return;
      }
      if (m_selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::Accent));
      } else if (m_hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::Fill3));
      } else {
        m_row->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_row->setBorder(colorSpecFromRole(ColorRole::Accent), Style::focusRingWidth);
      } else {
        m_row->clearBorder();
      }

      const auto activeRole = m_selected ? ColorRole::OnAccent : ColorRole::Label1;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::Label1);
      const ColorSpec mutedForeground =
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::Label2);
      m_badgeLabel->setColor(foreground);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_subtitle->setColor(mutedForeground);
      m_pinnedGlyph->setColor(mutedForeground);
      m_originGlyph->setColor(mutedForeground);
    }

    LauncherListStyle m_style{};
    Label* m_openHint = nullptr;
    float m_rowHeight = 0.0F;
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_row = nullptr;
    Label* m_badgeLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textCol = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_thumbnail = false;
    bool m_badgeVisible = false;
    bool m_reorderTarget = false;
  };

  class LauncherAppGridTile final : public Node {
  public:
    LauncherAppGridTile(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float gap = Style::spaceXs * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      auto col = ui::column({
          .out = &m_col,
          .align = FlexAlign::Center,
          .gap = gap,
          .paddingV = gap,
          .paddingH = padding,
          .radius = Style::scaledRadiusMd(m_style.scale),
          .fillWidth = true,
          .fillHeight = true,
      });
      addChild(std::move(col));

      addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::Label2),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      m_col->addChild(
          ui::box({
              .out = &m_badgeHost,
              .fill = clearColorSpec(),
              .width = launcherGridIconSize(m_style),
              .height = launcherGridIconSize(m_style),
              .visible = false,
              .participatesInLayout = false,
          })
      );
      m_badgeHost->addChild(
          ui::label({
              .out = &m_badgeLabel,
              .baselineMode = LabelBaselineMode::InkCentered,
              .participatesInLayout = false,
          })
      );

      m_col->addChild(
          ui::image({
              .out = &m_image,
              .visible = false,
          })
      );

      m_col->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = launcherGridIconSize(m_style),
              .color = colorSpecFromRole(ColorRole::Label1),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this] {
        if (!m_style.showIcons
            || m_badgeVisible
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_col->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .fontWeight = FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 2,
              .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      auto& accessible = accessibleState();
      accessible.role = AccessibleRole::ListItem;
      accessible.name = result.title;
      accessible.description = result.subtitle;
      accessible.selected = selected;
      accessible.actionName = "activate";
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherGridIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));
      m_badgeVisible = !result.badge.empty();

      setSize(width, height);
      m_col->setSize(width, height);
      m_isAppIcon = !result.desktopEntryPath.empty();

      const float pinSize = Style::fontSizeBody * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      m_pinnedGlyph->setGlyphSize(pinSize);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, padding);
      m_pinnedGlyph->setFrameSize(pinSize, pinSize);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(pinSize);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, height - padding - pinSize);
      m_originGlyph->setFrameSize(pinSize, pinSize);

      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);
      m_badgeHost->setVisible(m_badgeVisible);
      m_badgeHost->setParticipatesInLayout(m_badgeVisible);

      if (m_badgeVisible) {
        m_image->clear(renderer);
        m_badgeHost->setFrameSize(iconSize, iconSize);
        m_badgeLabel->setText(singleLinePreview(result.badge));
        m_badgeLabel->setFontSize(iconSize * 0.8F);
        m_badgeLabel->setMaxWidth(iconSize);
        m_badgeLabel->measure(renderer);
        m_badgeLabel->setPosition(
            (iconSize - m_badgeLabel->width()) * 0.5F, (iconSize - m_badgeLabel->height()) * 0.5F
        );
      } else if (m_style.showIcons) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        m_image->setSize(iconSize, iconSize);
        m_glyph->setGlyphSize(iconSize);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0F;
      const float textWidth = std::max(0.0F, width - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_badgeVisible || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_isAppIcon ? m_style.appIconColorizeTint : std::nullopt);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      const float iconSize = launcherGridIconSize(m_style);
      m_image->setSize(iconSize, iconSize);
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      m_col->setSize(width(), height());
      if (m_style.showIcons && !m_badgeVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_col->setFill(colorSpecFromRole(ColorRole::Accent));
      } else if (m_hovered) {
        m_col->setFill(colorSpecFromRole(ColorRole::Fill3));
      } else {
        m_col->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_col->setBorder(colorSpecFromRole(ColorRole::Accent), Style::focusRingWidth);
      } else {
        m_col->clearBorder();
      }

      const auto activeRole = m_selected ? ColorRole::OnAccent : ColorRole::Label1;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::Label1);
      m_badgeLabel->setColor(foreground);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_pinnedGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::Label2)
      );
      m_originGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::Label2)
      );
    }

    LauncherListStyle m_style{};
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_col = nullptr;
    Box* m_badgeHost = nullptr;
    Label* m_badgeLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    Label* m_title = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_badgeVisible = false;
    bool m_isAppIcon = false;
    bool m_reorderTarget = false;
  };

  struct LauncherSectionRowData {
    enum class Kind : std::uint8_t { Header, Applications };

    Kind kind = Kind::Header;
    std::string label;
    std::string glyphName;
    std::array<std::size_t, kMaxSectionColumns> resultIndices{};
    std::size_t resultCount = 0;
  };

  class LauncherSectionRow final : public Node {
  public:
    using SelectCallback = std::function<void(std::size_t)>;
    using ActivateCallback = std::function<void(std::size_t)>;
    using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

    LauncherSectionRow(
        LauncherListStyle style, AsyncTextureCache* asyncTextures, SelectCallback onSelect, ActivateCallback onActivate,
        SecondaryActivateCallback onSecondaryActivate
    )
        : m_style(style), m_asyncTextures(asyncTextures), m_onSelect(std::move(onSelect)),
          m_onActivate(std::move(onActivate)), m_onSecondaryActivate(std::move(onSecondaryActivate)) {
      auto header = ui::row({
          .out = &m_header,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * m_style.scale,
          .paddingH = Style::spaceXs * m_style.scale,
      });
      header->addChild(
          ui::glyph({
              .out = &m_headerGlyph,
              .glyphSize = Style::fontSizeTitle * m_style.scale,
              .color = colorSpecFromRole(ColorRole::AccentText),
          })
      );
      header->addChild(
          ui::label({
              .out = &m_headerLabel,
              .fontSize = Style::fontSizeBody * m_style.scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::Label1),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      );
      addChild(std::move(header));

      for (std::size_t slot = 0; slot < kMaxSectionColumns; ++slot) {
        auto area = std::make_unique<InputArea>();
        area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
        area->setOnEnter([this](const InputArea::PointerData&) { markLayoutDirty(); });
        area->setOnLeave([this] { markLayoutDirty(); });
        area->setOnPress([this, slot](const InputArea::PointerData& data) {
          if (!m_boundResultIndices[slot].has_value()) {
            return;
          }
          const std::size_t resultIndex = *m_boundResultIndices[slot];
          if (data.button == BTN_LEFT) {
            if (data.pressed) {
              if (m_onSelect) {
                m_onSelect(resultIndex);
              }
            } else if (m_onActivate) {
              m_onActivate(resultIndex);
            }
          } else if (data.button == BTN_RIGHT && data.pressed) {
            if (m_onSelect) {
              m_onSelect(resultIndex);
            }
            if (m_onSecondaryActivate) {
              m_onSecondaryActivate(resultIndex, data.sceneX, data.sceneY);
            }
          }
        });
        area->setTooltipProvider([this, slot] -> TooltipContent {
          if (m_results == nullptr || !m_boundResultIndices[slot].has_value()) {
            return {};
          }
          const std::size_t resultIndex = *m_boundResultIndices[slot];
          if (resultIndex >= m_results->size() || (*m_results)[resultIndex].origin.empty()) {
            return {};
          }
          return (*m_results)[resultIndex].origin;
        });

        auto tile = std::make_unique<LauncherAppGridTile>(m_style, asyncTextures);
        m_tiles[slot] = tile.get();
        area->addChild(std::move(tile));
        auto listRow = std::make_unique<LauncherResultRow>(m_style, asyncTextures);
        listRow->setVisible(false);
        m_listRows[slot] = listRow.get();
        auto emojiCell = ui::row({
            .out = &m_emojiCells[slot],
            .align = FlexAlign::Center,
            .justify = FlexJustify::Center,
            .radius = Tokens::radiusR8 * m_style.scale,
            .visible = false,
        });
        emojiCell->addChild(ui::label({
            .out = &m_emojiLabels[slot],
            .fontSize = Tokens::componentLauncherEmoji * m_style.scale,
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 1,
        }));
        emojiCell->addChild(ui::image({
            .out = &m_cellImages[slot],
            .visible = false,
            .participatesInLayout = false,
            .configure = [this](Image& image) { image.setRadius(Tokens::radiusR8 * m_style.scale); },
        }));
        area->addChild(std::move(emojiCell));
        area->addChild(std::move(listRow));
        m_areas[slot] = static_cast<InputArea*>(addChild(std::move(area)));
      }
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setGridView(bool gridView) { m_gridView = gridView; }
    void setCellGrid(CellGrid grid) { m_cellGrid = grid; }
    void setPillStyle(bool pill) {
      if (m_pillStyle == pill) {
        return;
      }
      m_pillStyle = pill;
      // Rulebook 9.14 header: footnote label_3 at inset 20 (the list sits at inset 8).
      m_header->setPadding(0.0F, (pill ? Tokens::spaceP12 : Style::spaceXs) * m_style.scale);
      m_headerLabel->setFontSize((pill ? Style::fontSizeCaption : Style::fontSizeBody) * m_style.scale);
      m_headerLabel->setFontWeight(pill ? FontWeight::Normal : FontWeight::SemiBold);
      m_headerLabel->setColor(colorSpecFromRole(pill ? ColorRole::Label3 : ColorRole::Label1));
    }

    void bind(
        const LauncherSectionRowData& row, const std::vector<LauncherResult>& results,
        std::optional<std::size_t> selectedResultIndex, float width, float height
    ) {
      m_results = &results;
      m_selectedResultIndex = selectedResultIndex;
      setSize(width, height);

      const bool headerVisible = row.kind == LauncherSectionRowData::Kind::Header;
      m_header->setVisible(headerVisible);
      m_header->setPosition(0.0F, 0.0F);
      m_header->setFrameSize(width, height);
      if (headerVisible) {
        if (!row.glyphName.empty()) {
          m_headerGlyph->setGlyph(row.glyphName);
        }
        m_headerGlyph->setVisible(!row.glyphName.empty());
        m_headerGlyph->setParticipatesInLayout(!row.glyphName.empty());
        m_headerLabel->setText(row.label);
        m_headerLabel->setMaxWidth(std::max(0.0F, width - Style::controlHeight * m_style.scale));
      }

      const bool cells = m_cellGrid != CellGrid::None;
      const std::size_t columns = cells ? cellColumns(m_cellGrid) : (m_gridView ? kAppGridColumns : 1);
      const float gap = m_gridView && !cells ? Style::spaceSm * m_style.scale : 0.0F;
      const auto columnCount = static_cast<float>(columns);
      const float cellWidth = std::max(0.0F, (width - static_cast<float>(columns - 1) * gap) / columnCount);
      for (std::size_t slot = 0; slot < kMaxSectionColumns; ++slot) {
        const bool active = !headerVisible && slot < row.resultCount && row.resultIndices[slot] < results.size();
        m_boundResultIndices[slot] = active ? std::optional<std::size_t>(row.resultIndices[slot]) : std::nullopt;
        m_areas[slot]->setVisible(active);
        m_tiles[slot]->setVisible(active && m_gridView && !cells);
        m_listRows[slot]->setVisible(active && !m_gridView && !cells);
        m_emojiCells[slot]->setVisible(active && cells);
        m_emojiLabels[slot]->setVisible(m_cellGrid == CellGrid::Emoji);
        m_cellImages[slot]->setVisible(false);
        if (!active) {
          continue;
        }
        const std::size_t visualSlot = Style::rtl() ? columns - 1 - slot : slot;
        m_areas[slot]->setPosition(static_cast<float>(visualSlot) * (cellWidth + gap), 0.0F);
        m_areas[slot]->setFrameSize(cellWidth, height);
        m_tiles[slot]->setPosition(0.0F, 0.0F);
        m_tiles[slot]->setSize(cellWidth, height);
        m_listRows[slot]->setPosition(0.0F, 0.0F);
        m_listRows[slot]->setSize(cellWidth, height);
        m_emojiCells[slot]->setPosition(0.0F, 0.0F);
        m_emojiCells[slot]->setFrameSize(cellWidth, height);
        const float inset = Tokens::spaceP4 * m_style.scale;
        m_cellImages[slot]->setPosition(inset, inset);
        m_cellImages[slot]->setSize(std::max(0.0F, cellWidth - 2.0F * inset), std::max(0.0F, height - 2.0F * inset));
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_header->visible()) {
        m_header->layout(renderer);
        return;
      }
      if (m_results == nullptr) {
        return;
      }
      for (std::size_t slot = 0; slot < kMaxSectionColumns; ++slot) {
        if (!m_boundResultIndices[slot].has_value()) {
          continue;
        }
        const std::size_t resultIndex = *m_boundResultIndices[slot];
        if (resultIndex >= m_results->size()) {
          continue;
        }
        if (m_cellGrid != CellGrid::None) {
          const bool selected = m_selectedResultIndex == resultIndex;
          const LauncherResult& result = (*m_results)[resultIndex];
          m_emojiLabels[slot]->setText(result.badge);
          if (m_cellGrid != CellGrid::Emoji && m_asyncTextures != nullptr && !result.iconPath.empty()) {
            Image& image = *m_cellImages[slot];
            image.setFit(m_cellGrid == CellGrid::Sticker ? ImageFit::Contain : ImageFit::Cover);
            const int target = static_cast<int>(std::round(std::max(image.width(), image.height())));
            image.setVisible(image.setSourceFileAsync(renderer, *m_asyncTextures, result.iconPath, target, true));
          }
          m_emojiCells[slot]->setFill(
              selected ? colorSpecFromRole(ColorRole::Lens)
                       : (m_areas[slot]->hovered() ? colorSpecFromRole(ColorRole::Fill1) : clearColorSpec())
          );
        } else if (m_gridView) {
          m_tiles[slot]->setListStyle(m_style);
          m_tiles[slot]->bind(
              renderer, (*m_results)[resultIndex], m_areas[slot]->width(), m_areas[slot]->height(),
              m_selectedResultIndex == resultIndex, m_areas[slot]->hovered()
          );
        } else {
          m_listRows[slot]->setListStyle(m_style);
          m_listRows[slot]->bind(
              renderer, (*m_results)[resultIndex], m_areas[slot]->width(), m_areas[slot]->height(),
              m_selectedResultIndex == resultIndex, m_areas[slot]->hovered()
          );
        }
        m_areas[slot]->layout(renderer);
      }
    }

  public:
    // The cell showing result `resultIndex`; nullptr when this row does not show it.
    [[nodiscard]] const Node* nodeForResult(std::size_t resultIndex) const {
      for (std::size_t slot = 0; slot < kMaxSectionColumns; ++slot) {
        if (m_boundResultIndices[slot] == resultIndex && m_areas[slot] != nullptr) {
          return m_areas[slot];
        }
      }
      return nullptr;
    }

  private:
    LauncherListStyle m_style{};
    bool m_pillStyle = false;
    Flex* m_header = nullptr;
    Glyph* m_headerGlyph = nullptr;
    Label* m_headerLabel = nullptr;
    CellGrid m_cellGrid = CellGrid::None;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::array<Image*, kMaxSectionColumns> m_cellImages{};
    std::array<InputArea*, kMaxSectionColumns> m_areas{};
    std::array<LauncherAppGridTile*, kMaxSectionColumns> m_tiles{};
    std::array<LauncherResultRow*, kMaxSectionColumns> m_listRows{};
    std::array<Flex*, kMaxSectionColumns> m_emojiCells{};
    std::array<Label*, kMaxSectionColumns> m_emojiLabels{};
    std::array<std::optional<std::size_t>, kMaxSectionColumns> m_boundResultIndices{};
    const std::vector<LauncherResult>* m_results = nullptr;
    std::optional<std::size_t> m_selectedResultIndex;
    SelectCallback m_onSelect;
    ActivateCallback m_onActivate;
    SecondaryActivateCallback m_onSecondaryActivate;
    bool m_gridView = false;
  };


  // The section cell that shows result `resultIndex` among the rows under `root`.
  inline const Node* sectionCellForResult(const Node& root, std::size_t resultIndex) {
    if (const auto* row = dynamic_cast<const LauncherSectionRow*>(&root); row != nullptr && row->visible()) {
      return row->nodeForResult(resultIndex);
    }
    for (const auto& child : root.children()) {
      if (const Node* cell = sectionCellForResult(*child, resultIndex); cell != nullptr) {
        return cell;
      }
    }
    return nullptr;
  }

} // namespace launcher_detail
