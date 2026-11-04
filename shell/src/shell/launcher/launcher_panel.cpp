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

namespace {

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
  const Node* cellIcon(const Node& cell) {
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

  [[nodiscard]] std::size_t cellColumns(CellGrid grid) {
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

  [[nodiscard]] float cellRowHeight(CellGrid grid) {
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
  [[nodiscard]] float launcherSheetHeight() {
    const float rows = static_cast<float>(Tokens::componentLauncherRows);
    return Tokens::componentLauncherSearch + 1.0F + 2.0F * Tokens::componentLauncherPadding
        + rows * Tokens::componentLauncherCellHeight + rows * Tokens::spaceP8 + Tokens::componentPageDotsRow;
  }
  [[nodiscard]] bool isAppsQuery(std::string_view query) { return query == "apps"; }
  [[nodiscard]] bool isTwoLineRow(const LauncherResult& result) { return result.presentation.starts_with("clipboard"); }
  [[nodiscard]] std::string formatByteSize(std::size_t bytes) {
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

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
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

  [[nodiscard]] std::string singleLinePreview(std::string_view text) {
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

  [[nodiscard]] bool isDetailPresentation(const LauncherResult& result) { return result.presentation == "detail"; }
  [[nodiscard]] bool isMediaPresentation(const LauncherResult& result) {
    return result.presentation == "gif" || result.presentation == "sticker" || result.presentation == "file-image";
  }

  [[nodiscard]] std::string providerOverviewId(std::string_view prefix) {
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

  [[nodiscard]] float launcherIconSize(const LauncherListStyle& style) {
    if (style.pill) {
      return Tokens::componentLauncherRowIcon * style.scale;
    }
    return (style.compact ? kIconSizeCompact : kIconSizeDefault) * style.scale;
  }

  [[nodiscard]] float rowGap(const LauncherListStyle& style) {
    if (style.pill) {
      return Tokens::spaceP12 * style.scale;
    }
    return (style.compact ? Style::spaceSm : Style::spaceMd) * style.scale;
  }

  [[nodiscard]] float launcherGridIconSize(const LauncherListStyle& style) { return Tokens::componentLauncherIcon * style.scale; }

  [[nodiscard]] float stableLabelHeight(const TextMetrics& metrics) { return std::round(metrics.bottom - metrics.top); }

  [[nodiscard]] float launcherTextStackHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float bodySize = Style::fontSizeBody * style.scale;
    float textHeight = stableLabelHeight(renderer.measureFont(bodySize, {FontWeight::SemiBold}));
    if (!style.compact) {
      const float captionSize = Style::fontSizeCaption * style.scale;
      textHeight += stableLabelHeight(renderer.measureFont(captionSize, {FontWeight::Normal}));
    }
    return textHeight;
  }

  [[nodiscard]] float launcherRowHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float textHeight = launcherTextStackHeight(renderer, style);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] float launcherRowHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5F : Style::spaceXs) * style.scale;
    const float bodySize = Style::fontSizeBody * style.scale;
    const float captionSize = Style::fontSizeCaption * style.scale;
    const float textHeight = bodySize + (style.compact ? 0.0F : captionSize);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] LauncherListStyle
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

} // namespace

namespace {
  // The section cell that shows result `resultIndex` among the rows under `root`.
  const Node* sectionCellForResult(const Node& root, std::size_t resultIndex) {
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
} // namespace

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherResultAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->setListStyle(m_style);
    row->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets> itemTooltipAnchorInsets(
      std::size_t index, float cellWidth, float cellHeight, bool /*overlayHovered*/
  ) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float pinnedWidth = (*m_results)[index].pinned ? glyphSize + gap : 0.0F;
    const float left = std::max(padding, cellWidth - padding - pinnedWidth - glyphSize);
    const float top = std::max(0.0F, (cellHeight - glyphSize) * 0.5F);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;

    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherAppGridAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherAppGridAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherAppGridTile>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* gridTile = static_cast<LauncherAppGridTile*>(&tile);
    gridTile->setListStyle(m_style);
    gridTile->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    gridTile->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets> itemTooltipAnchorInsets(
      std::size_t index, float cellWidth, float cellHeight, bool /*overlayHovered*/
  ) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float left = std::max(0.0F, cellWidth - padding - glyphSize);
    const float top = std::max(0.0F, cellHeight - padding - glyphSize);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;
    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  PointerReleaseResult onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value();
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    }
    return {
        .rebind = sourceIndex.has_value() || targetIndex.has_value(),
        .activateIndex = activated ? sourceIndex : std::nullopt,
    };
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherSectionsAdapter final : public VirtualListAdapter {
public:
  using SelectCallback = std::function<void(std::size_t)>;
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherSectionsAdapter(
      LauncherListStyle style, AsyncTextureCache* cache, SelectCallback onSelect, ActivateCallback onActivate,
      SecondaryActivateCallback onSecondaryActivate
  )
      : m_style(style), m_cache(cache), m_onSelect(std::move(onSelect)), m_onActivate(std::move(onActivate)),
        m_onSecondaryActivate(std::move(onSecondaryActivate)) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setSelectedResultIndex(std::optional<std::size_t> index) { m_selectedResultIndex = index; }
  [[nodiscard]] bool setGridView(bool gridView) {
    if (m_gridView == gridView) {
      return false;
    }
    m_gridView = gridView;
    return true;
  }
  void setApplicationRowHeight(float height) {
    const float next = std::max(1.0F, height);
    if (std::abs(m_applicationRowHeight - next) < 0.5F) {
      return;
    }
    m_applicationRowHeight = next;
    ++m_revision;
  }

  struct Section {
    std::string label;
    std::string glyphName;
    std::vector<std::size_t> resultIndices;
  };

  void rebuild(const std::vector<LauncherCategory>& categories, std::string otherLabel) {
    std::vector<Section> sections;
    const std::size_t count = m_results == nullptr ? 0U : m_results->size();
    std::vector<bool> assigned(count, false);
    for (const LauncherCategory& category : categories) {
      Section section{.label = category.label, .glyphName = category.glyphName, .resultIndices = {}};
      for (std::size_t i = 0; i < count; ++i) {
        if (!assigned[i] && (*m_results)[i].category == category.label) {
          section.resultIndices.push_back(i);
          assigned[i] = true;
        }
      }
      sections.push_back(std::move(section));
    }
    Section other{.label = std::move(otherLabel), .glyphName = "apps", .resultIndices = {}};
    for (std::size_t i = 0; i < count; ++i) {
      if (!assigned[i]) {
        other.resultIndices.push_back(i);
      }
    }
    sections.push_back(std::move(other));
    rebuildSections(sections);
  }

  void rebuildSections(const std::vector<Section>& sections) {
    m_rows.clear();
    m_visualResultOrder.clear();
    m_resultPositions.assign(m_results == nullptr ? 0U : m_results->size(), std::nullopt);
    ++m_revision;
    if (m_results == nullptr) {
      return;
    }
    const std::size_t columns =
        m_cellGrid != CellGrid::None ? cellColumns(m_cellGrid) : (m_gridView ? kAppGridColumns : 1);
    for (const Section& section : sections) {
      if (section.resultIndices.empty()) {
        continue;
      }
      LauncherSectionRowData header;
      header.kind = LauncherSectionRowData::Kind::Header;
      header.label = section.label;
      header.glyphName = m_pillStyle ? std::string() : section.glyphName;
      m_rows.push_back(std::move(header));

      for (std::size_t offset = 0; offset < section.resultIndices.size(); offset += columns) {
        LauncherSectionRowData row;
        row.kind = LauncherSectionRowData::Kind::Applications;
        row.resultCount = std::min(columns, section.resultIndices.size() - offset);
        const std::size_t rowIndex = m_rows.size();
        for (std::size_t column = 0; column < row.resultCount; ++column) {
          const std::size_t resultIndex = section.resultIndices[offset + column];
          row.resultIndices[column] = resultIndex;
          m_resultPositions[resultIndex] = ResultPosition{
              .row = rowIndex,
              .column = column,
              .visualOrder = m_visualResultOrder.size(),
          };
          m_visualResultOrder.push_back(resultIndex);
        }
        m_rows.push_back(std::move(row));
      }
    }
  }

  void setCellGrid(CellGrid grid) {
    if (m_cellGrid != grid) {
      m_cellGrid = grid;
      ++m_revision;
    }
  }

  // Rulebook 9.14 sections: plain footnote headers h 28, rows h 40.
  void setPillStyle(bool pill) {
    if (m_pillStyle != pill) {
      m_pillStyle = pill;
      ++m_revision;
    }
  }

  // Unscaled height of a result row: a media cell row, a two-line row or a plain row.
  [[nodiscard]] float resultRowHeight(const LauncherSectionRowData& row) const {
    if (m_cellGrid == CellGrid::None && m_results != nullptr && row.resultCount > 0
        && row.resultIndices[0] < m_results->size() && isTwoLineRow((*m_results)[row.resultIndices[0]])) {
      return Tokens::componentLauncherRowTall;
    }
    return cellRowHeight(m_cellGrid);
  }

  // Height of the first `maxRows` result rows and the headers between them.
  [[nodiscard]] float pillContentHeight(std::size_t maxRows) const {
    float height = 0.0F;
    std::size_t rows = 0;
    // Taller media cells show fewer rows in the same space.
    maxRows = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<float>(maxRows) * Tokens::componentLauncherRow / cellRowHeight(m_cellGrid))
    );
    for (const auto& row : m_rows) {
      if (row.kind == LauncherSectionRowData::Kind::Header) {
        height += Tokens::componentLauncherSectionHeader * m_style.scale;
      } else if (rows++ < maxRows) {
        height += resultRowHeight(row) * m_style.scale;
      } else {
        break;
      }
    }
    return height;
  }

  [[nodiscard]] std::optional<std::size_t> rowForHeader(std::string_view label) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
      if (m_rows[i].kind == LauncherSectionRowData::Kind::Header && m_rows[i].label == label) {
        return i;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> firstResultIndex() const {
    return m_visualResultOrder.empty() ? std::nullopt : std::optional<std::size_t>(m_visualResultOrder.front());
  }

  [[nodiscard]] std::optional<std::size_t> rowForResult(std::size_t resultIndex) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return std::nullopt;
    }
    return m_resultPositions[resultIndex]->row;
  }

  [[nodiscard]] std::optional<std::size_t> moveHorizontal(std::size_t resultIndex, int direction) const {
    if (!m_gridView && m_cellGrid == CellGrid::None) {
      return resultIndex < m_resultPositions.size() && m_resultPositions[resultIndex].has_value()
          ? std::optional<std::size_t>(resultIndex)
          : firstResultIndex();
    }
    if (resultIndex >= m_resultPositions.size()
        || !m_resultPositions[resultIndex].has_value()
        || m_visualResultOrder.empty()) {
      return firstResultIndex();
    }
    const std::size_t order = m_resultPositions[resultIndex]->visualOrder;
    if (direction < 0) {
      return order == 0 ? std::optional<std::size_t>(resultIndex)
                        : std::optional<std::size_t>(m_visualResultOrder[order - 1]);
    }
    return order + 1 >= m_visualResultOrder.size() ? std::optional<std::size_t>(resultIndex)
                                                   : std::optional<std::size_t>(m_visualResultOrder[order + 1]);
  }

  [[nodiscard]] std::optional<std::size_t>
  moveVertical(std::size_t resultIndex, int direction, std::size_t applicationRows = 1) const {
    if (resultIndex >= m_resultPositions.size() || !m_resultPositions[resultIndex].has_value()) {
      return firstResultIndex();
    }
    const ResultPosition position = *m_resultPositions[resultIndex];
    std::size_t rowIndex = position.row;
    std::size_t rowsRemaining = std::max<std::size_t>(1, applicationRows);
    while (true) {
      if (direction < 0) {
        if (rowIndex == 0) {
          return resultIndex;
        }
        --rowIndex;
      } else {
        if (rowIndex + 1 >= m_rows.size()) {
          return resultIndex;
        }
        ++rowIndex;
      }
      const auto& row = m_rows[rowIndex];
      if (row.kind != LauncherSectionRowData::Kind::Applications || row.resultCount == 0) {
        continue;
      }
      --rowsRemaining;
      if (rowsRemaining == 0) {
        return row.resultIndices[std::min(position.column, row.resultCount - 1)];
      }
    }
  }

  [[nodiscard]] std::size_t estimatedVisibleApplicationRows(float viewportHeight) const {
    const float rowHeight = std::max(1.0F, m_applicationRowHeight);
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(viewportHeight / rowHeight)));
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_rows.size(); }
  [[nodiscard]] std::uint64_t itemRevision(std::size_t /*index*/) const override { return m_revision; }
  [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return false; }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_rows.size()) {
      return 1.0F;
    }
    if (m_pillStyle) {
      return (m_rows[index].kind == LauncherSectionRowData::Kind::Header ? Tokens::componentLauncherSectionHeader
                                                                         : resultRowHeight(m_rows[index]))
          * m_style.scale;
    }
    if (m_rows[index].kind == LauncherSectionRowData::Kind::Header) {
      const float textHeight =
          stableLabelHeight(renderer.measureFont(Style::fontSizeBody * m_style.scale, {FontWeight::SemiBold}));
      return std::ceil(std::max(Style::fontSizeTitle * m_style.scale, textHeight) + Style::spaceSm * m_style.scale);
    }

    (void)width;
    return m_applicationRowHeight;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<LauncherSectionRow>(m_style, m_cache, m_onSelect, m_onActivate, m_onSecondaryActivate);
  }

  void bindItem(Renderer& /*renderer*/, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (m_results == nullptr || index >= m_rows.size()) {
      return;
    }
    auto* row = static_cast<LauncherSectionRow*>(&item);
    row->setListStyle(m_style);
    row->setGridView(m_gridView);
    row->setCellGrid(m_cellGrid);
    row->setPillStyle(m_pillStyle);
    row->bind(m_rows[index], *m_results, m_selectedResultIndex, width, item.height());
  }

private:
  struct ResultPosition {
    std::size_t row = 0;
    std::size_t column = 0;
    std::size_t visualOrder = 0;
  };

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  SelectCallback m_onSelect;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  const std::vector<LauncherResult>* m_results = nullptr;
  std::vector<LauncherSectionRowData> m_rows;
  std::vector<std::optional<ResultPosition>> m_resultPositions;
  std::vector<std::size_t> m_visualResultOrder;
  std::optional<std::size_t> m_selectedResultIndex;
  std::uint64_t m_revision = 0;
  float m_applicationRowHeight = 1.0F;
  bool m_gridView = false;
  CellGrid m_cellGrid = CellGrid::None;
  bool m_pillStyle = false;
};

LauncherPanel::LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures)
    : m_iconResolver(true), m_config(config), m_accounts(accounts), m_asyncTextures(asyncTextures) {}

LauncherPanel::~LauncherPanel() = default;

bool LauncherPanel::usesBrowserLayout() const {
  return m_scopedProviderId.empty()
      && (m_config == nullptr || m_config->config().shell.launcher.layout == ShellConfig::LauncherLayout::Full);
}

float LauncherPanel::preferredWidth() const { return scaled(usesBrowserLayout() ? Tokens::componentLauncherWidth : 560.0F); }

float LauncherPanel::preferredHeight() const { return scaled(usesBrowserLayout() ? launcherSheetHeight() : 500.0F); }

float LauncherPanel::measureContentHeight(Renderer& /*renderer*/, float /*contentWidth*/) {
  const float scale = contentScale();
  const float field = Tokens::componentLauncherSearch * scale;
  // Hybrid material: liquid glass on the empty pill, frosted once it grows into the sheet.
  const float liquid = usesBrowserLayout() && !pillExpanded() ? 1.0F : 0.0F;
  if (m_sentLiquid != liquid) {
    const bool animate = m_sentLiquid >= 0.0F;
    m_sentLiquid = liquid;
    PanelManager::instance().setPanelLiquid(liquid, animate);
  }
  if (!pillExpanded()) {
    return field;
  }
  if (!shouldUseLauncherSections() || m_launcherSectionsAdapter == nullptr || m_results.empty()) {
    return preferredHeight();
  }
  // Field, the taller of the list (p 8, 8 rows visible) and the preview, footer (rulebook 9.14, direction C).
  const bool strip = (m_emojiStrip != nullptr && m_emojiStrip->visible()) || (m_packStrip != nullptr && m_packStrip->visible());
  const float list = 2.0F * Tokens::spaceP8 * scale + m_launcherSectionsAdapter->pillContentHeight(8)
      + (strip ? Tokens::componentLauncherTabs * scale : 0.0F);
  // A clipboard image replaces the 64 icon with a 160 high picture.
  const bool picture = m_selectedIndex < m_results.size()
      && (m_results[m_selectedIndex].presentation == "clipboard-image" || isMediaPresentation(m_results[m_selectedIndex]));
  const float previewHeight = (Tokens::componentLauncherPreviewMin
                               + (picture ? Tokens::componentLauncherPreviewMedia - Tokens::componentLauncherIcon : 0.0F))
      * scale;
  // Mode tabs and the chip row (file kinds, sticker packs) each add a row and a hairline.
  const float rows = (m_modeProviderId.empty() ? 0.0F : 1.0F) + (m_kindTabs != nullptr && m_kindTabs->visible() ? 1.0F : 0.0F);
  const float hairlines = 2.0F + rows; // under the field (and each row) and above the footer
  return field + hairlines + rows * Tokens::componentLauncherTabs * scale + std::max(list, previewHeight)
      + Tokens::componentLauncherFooter * scale;
}

PanelPlacement LauncherPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.launcherPlacement : PanelPlacement::Floating;
}

void LauncherPanel::applyProviderConfig(LauncherProvider& provider) const {
  std::string triggerWord = std::string(provider.defaultPrefix());
  std::string prefix = "/";
  std::optional<bool> global;
  if (m_config != nullptr) {
    const auto& launcherCfg = m_config->config().shell.launcher;
    prefix = launcherCfg.providerPrefix;
    if (provider.allowCustomPrefix()) {
      const std::string key = StringUtils::toLower(provider.id());
      auto it = std::ranges::find(launcherCfg.providers, key, &LauncherProviderConfig::name);
      if (it != launcherCfg.providers.end()) {
        if (!it->prefix.empty()) {
          triggerWord = it->prefix;
        }
        global = it->global;
      }
    }
  }

  if (provider.allowCustomPrefix()) {
    provider.setCustomPrefix(triggerWord.empty() ? std::string() : prefix + triggerWord);
    provider.setCustomIncludeInGlobalSearch(global);
  }
}

void LauncherPanel::finishActivation(
    LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch
) {
  const bool trackUsage = m_config == nullptr || m_config->config().shell.launcher.trackUsage;
  if (trackUsage && provider.trackUsage()) {
    m_usageTracker.record(provider.id(), resultId);
  }
  PanelManager::instance().closePanel(false);
  if (copied && provider.supportsAutoPaste() && m_onCopiedActivation) {
    m_onCopiedActivation();
  }
  if (appLaunch && m_onAppLaunch) {
    m_onAppLaunch();
  }
}

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  applyProviderConfig(*provider);
  provider->initialize();
  LauncherProvider* providerPtr = provider.get();
  provider->setResultsChangedCallback([this, providerPtr] { onProviderResultsChanged(*providerPtr); });
  provider->setQueryRequestedCallback([this](const std::string& query) { setQuery(query); });
  provider->setActivationDoneCallback([this, providerPtr](const std::string& resultId, bool copied) {
    finishActivation(*providerPtr, resultId, copied);
  });
  m_providers.push_back(std::move(provider));
}

void LauncherPanel::clearDynamicProviders() {
  std::erase_if(m_providers, [](const std::unique_ptr<LauncherProvider>& provider) { return provider->isDynamic(); });
}

void LauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const std::unique_ptr<LauncherProvider>& provider) {
    return provider->id().starts_with(prefix);
  });
}

void LauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
}

void LauncherPanel::create() {
  m_launcherRowHeight = 0.0F;
  m_browserMode = usesBrowserLayout();
  const float scale = contentScale();

  auto input = ui::input({
      .out = &m_input,
      .placeholder = m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder,
      .fontSize = (m_browserMode ? Tokens::typeTitle3.size : Style::fontSizeBody) * scale,
      .controlHeight = (m_browserMode ? Tokens::componentLauncherSearch : Style::controlHeight) * scale,
      .horizontalPadding = m_browserMode ? 0.0F : Style::spaceMd * scale,
      .clearButtonEnabled = !m_browserMode,
      .frameVisible = !m_browserMode,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](const std::string& text) {
            if (m_launcherProviderViewId == kSessionProviderId) {
              m_sessionReturnView.reset();
            }
            m_launcherProviderViewId.clear();
            onInputChanged(text);
            if (m_input == nullptr) {
              return;
            }
            const std::string preview = singleLinePreview(text);
            if (preview != text) {
              m_input->setValue(preview);
            }
          },
      .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
  });
  if (m_input != nullptr && m_input->inputArea() != nullptr) {
    m_input->inputArea()->setTabFocusKey("launcher.search");
  }

  auto categoryFilter = ui::segmented({
      .out = &m_categoryFilter,
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .equalSegmentWidths = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
  });

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  if (m_browserMode) {
    body->setGap(Style::spaceSm * scale);
  }

  auto resultsViewport = ui::column({
      .out = &m_resultsViewport,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  const LauncherListStyle initialStyle = launcherListStyleFrom(m_browserMode, m_config, scale, panelCardOpacity());
  m_listAdapter = std::make_unique<LauncherResultAdapter>(initialStyle, m_asyncTextures);
  m_gridAdapter = std::make_unique<LauncherAppGridAdapter>(initialStyle, m_asyncTextures);
  m_listAdapter->setResults(&m_results);
  m_gridAdapter->setResults(&m_results);
  const auto onActivate = [this](std::size_t index) { activateAt(index); };
  const auto onSecondaryActivate = [this](std::size_t index, float ax, float ay) {
    (void)openAppActionsMenu(index, ax, ay);
  };
  m_launcherSectionsAdapter = std::make_unique<LauncherSectionsAdapter>(
      initialStyle, m_asyncTextures, [this](std::size_t index) { selectResult(index, false); }, onActivate,
      onSecondaryActivate
  );
  m_launcherSectionsAdapter->setResults(&m_results);
  m_listAdapter->setOnActivate(onActivate);
  m_listAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_listAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });
  m_gridAdapter->setOnActivate(onActivate);
  m_gridAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_gridAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });

  resultsViewport->addChild(
      ui::virtualListView({
          .out = &m_launcherSections,
          .contentScale = scale,
          .itemGap = Style::spaceXs * scale,
          .overscanItems = kRowOverscan,
          .adapter = m_launcherSectionsAdapter.get(),
          .flexGrow = 1.0F,
          .visible = false,
          .participatesInLayout = false,
          .configure = [pill = m_browserMode](VirtualListView& list) {
            list.setFillWidth(true);
            list.scrollView().setReserveScrollbarGutter(!pill);
          },
      })
  );

  // Stickers mode: pack chips under the grid; a click filters to that pack (DESIGN.md media modes).
  resultsViewport->addChild(ui::row({
      .out = &m_packStrip,
      .align = FlexAlign::Center,
      .gap = Tokens::spaceP4 * scale,
      .paddingH = Tokens::spaceP4 * scale,
      .minHeight = Tokens::componentLauncherTabs * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  }));

  // Emoji mode: one button per group under the grid; a click scrolls to that group (DESIGN.md media modes).
  resultsViewport->addChild(ui::row({
      .out = &m_emojiStrip,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .paddingH = Tokens::spaceP4 * scale,
      .minHeight = Tokens::componentLauncherTabs * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  }));

  resultsViewport->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeightEstimate(initialStyle),
          .squareCells = false,
          .columnGap = 0.0F,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kRowOverscan,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );
  m_grid->setOnPageChanged([this](std::size_t page, std::size_t pages) { updatePageDots(page, pages); });

  resultsViewport->addChild(
      ui::row({
          .out = &m_pageDotsRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Tokens::componentPageDotsGap * scale,
          .minHeight = Tokens::componentPageDotsRow * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  auto detailScroll = ui::scrollView({
      .out = &m_detailScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceSm * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity);
      },
  });
  auto* detailContent = detailScroll->content();
  detailContent->setDirection(FlexDirection::Vertical);
  detailContent->setAlign(FlexAlign::Stretch);
  detailContent->setGap(Style::spaceSm * scale);
  detailContent->addChild(
      ui::label({
          .out = &m_detailSubtitle,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  detailContent->addChild(
      ui::label({
          .out = &m_detailBody,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 0,
          .flexGrow = 1.0F,
      })
  );
  resultsViewport->addChild(std::move(detailScroll));

  resultsViewport->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .visible = false,
          .participatesInLayout = false,
      })
  );
  body->addChild(std::move(resultsViewport));

  if (m_browserMode) {
    // The panel does not pad this content (padsContent): the field fills the 56 pill edge to edge.
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .fillWidth = true,
        .fillHeight = true,
    });

    // Rulebook 9.14: pill h 56, symbol 20 label_2 at inset 20, g 12, title-3 field.
    input->setFlexGrow(1.0F);
    // DESIGN.md "Show all" B: fill_1 capsule h 24, footnote label_1, x symbol; a click leaves the scope.
    const auto filesToken = [this, scale] {
      auto token = ui::row({
          .out = &m_filesToken,
          .align = FlexAlign::Center,
          .gap = Tokens::spaceP4 * scale,
          .paddingH = Tokens::spaceP8 * scale,
          .fill = colorSpecFromRole(ColorRole::Fill1),
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
          .visible = m_filesScope,
          .participatesInLayout = m_filesScope,
      });
      token->addChild(ui::label({
          .out = &m_filesTokenLabel,
          .text = i18n::tr("launcher.files.title"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label1),
      }));
      token->addChild(ui::glyph({.glyph = "x", .glyphSize = Tokens::sizeSymbolInline * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setOnClick([this](const InputArea::PointerData&) { setFilesScope(false); });
      token->addChild(std::move(area));
      return token;
    };
    container->addChild(ui::row(
        {.align = FlexAlign::Center,
         .gap = Tokens::spaceP12 * scale,
         .paddingH = Tokens::spaceP20 * scale,
         .minHeight = Tokens::componentLauncherSearch * scale,
         .fillWidth = true},
        ui::glyph(
            {.out = &m_pillSearchGlyph,
             .glyph = "search",
             .glyphSize = Tokens::sizeSymbolL * scale,
             .color = colorSpecFromRole(ColorRole::Label2)}
        ),
        filesToken(),
        std::move(input)
    ));
    container->addChild(ui::separator({
        .out = &m_pillSeparator,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::row({
        .out = &m_modeTabs,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .minHeight = Tokens::componentLauncherTabs * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::separator({
        .out = &m_modeTabsLine,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    m_modeTabItems.clear();
    container->addChild(ui::row({
        .out = &m_kindTabs,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .minHeight = Tokens::componentLauncherTabs * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::separator({
        .out = &m_kindTabsLine,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    m_kindTabItems.clear();
    body->setPadding(Tokens::componentLauncherPadding * scale);
    body->setVisible(false);
    body->setParticipatesInLayout(false);

    // Direction C: list | preview. The preview shows the current row: icon 64, title-3, kind, actions, details.
    body->setDirection(FlexDirection::Horizontal);
    body->setAlign(FlexAlign::Stretch);
    body->setGap(0.0F);
    body->addChild(ui::separator({
        .out = &m_previewDivider,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .orientation = SeparatorOrientation::VerticalRule,
        .visible = false,
        .participatesInLayout = false,
    }));
    auto preview = ui::column({
        .out = &m_previewPane,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * scale,
        .paddingV = Tokens::spaceP24 * scale,
        .paddingH = Tokens::spaceP20 * scale,
        .flexGrow = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    });
    const float previewIcon = Tokens::componentLauncherIcon * scale;
    preview->addChild(ui::image({.out = &m_previewImage, .width = previewIcon, .height = previewIcon, .visible = false}));
    preview->addChild(ui::glyph({
        .out = &m_previewGlyph,
        .glyphSize = previewIcon * 0.75F,
        .color = colorSpecFromRole(ColorRole::Label1),
        .visible = false,
    }));
    preview->addChild(ui::label({
        .out = &m_previewEmoji,
        .fontSize = Tokens::componentLauncherEmojiPreview * scale,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .visible = false,
        .participatesInLayout = false,
    }));
    preview->addChild(ui::label({
        .out = &m_previewTitle,
        .fontSize = Tokens::typeTitle3.size * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 2,
        .ellipsize = TextEllipsize::End,
    }));
    preview->addChild(ui::label({
        .out = &m_previewKind,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    // A new preview image holds no picture yet: the next selection decodes again.
    m_previewShotId.clear();
    preview->addChild(ui::image({
        .out = &m_previewShot,
        .width = Tokens::componentLauncherPreviewMedia * scale * 1.75F,
        .height = Tokens::componentLauncherPreviewMedia * scale,
        .visible = false,
        .participatesInLayout = false,
        .configure = [scale](Image& image) {
          image.setFit(ImageFit::Contain);
          image.setRadius(Tokens::radiusR12 * scale);
        },
    }));
    // Clipboard text: monospace in a fill_1 box (.agent/mockups/launcher-media.html clipboard).
    preview->addChild(ui::column(
        {
            .out = &m_previewBodyBox,
            .align = FlexAlign::Start,
            .padding = Tokens::spaceP12 * scale,
            .fill = colorSpecFromRole(ColorRole::Fill1),
            .radius = Tokens::radiusR8 * scale,
            .minHeight = Tokens::componentLauncherPreviewMedia * 0.75F * scale,
            .fillWidth = true,
            .visible = false,
            .participatesInLayout = false,
        },
        ui::label({
            .out = &m_previewBody,
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string("monospace"),
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 6,
            .ellipsize = TextEllipsize::End,
        })
    ));
    auto tones = ui::row({
        .out = &m_previewTones,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .visible = false,
        .participatesInLayout = false,
    });
    for (std::size_t i = 0; i < m_previewToneLabels.size(); ++i) {
      auto cell = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .radius = Tokens::radiusR8 * scale,
          .minWidth = Tokens::sizeControlM * scale,
          .minHeight = Tokens::sizeControlM * scale,
      });
      cell->addChild(ui::label({.out = &m_previewToneLabels[i], .fontSize = Tokens::componentLauncherEmoji * scale, .maxLines = 1}));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setFrameSize(Tokens::sizeControlM * scale, Tokens::sizeControlM * scale);
      area->setOnClick([this, i](const InputArea::PointerData&) { activateEmojiVariant(i); });
      cell->addChild(std::move(area));
      tones->addChild(std::move(cell));
    }
    preview->addChild(std::move(tones));
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale});
    actions->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
    actions->addChild(ui::button({
        .out = &m_previewOpen,
        .text = i18n::tr("launcher.pill.open-hint"),
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Primary,
        .onClick = [this] { activateSelected(); },
    }));
    actions->addChild(ui::button({
        .out = &m_previewAction,
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Default,
        .visible = false,
        .participatesInLayout = false,
        .onClick = [this] { activatePreviewAction(); },
    }));
    actions->addChild(ui::button({
        .out = &m_previewDelete,
        .text = i18n::tr("launcher.clipboard.delete"),
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Default,
        .visible = false,
        .participatesInLayout = false,
        .onClick = [this] { deleteSelectedClipboardEntry(); },
    }));
    preview->addChild(std::move(actions));
    auto meta = ui::column({.align = FlexAlign::Stretch, .fillWidth = true});
    meta->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
    for (std::size_t i = 0; i < m_previewMetaRows.size(); ++i) {
      auto metaRow = ui::row({
          .out = &m_previewMetaRows[i],
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .minHeight = Tokens::sizeControlM * scale,
          .fillWidth = true,
          .visible = false,
          .participatesInLayout = false,
      });
      metaRow->addChild(ui::label({.out = &m_previewMetaKeys[i], .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
      metaRow->addChild(ui::label({.out = &m_previewMetaValues[i], .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::End}));
      meta->addChild(std::move(metaRow));
    }
    preview->addChild(std::move(meta));
    body->addChild(std::move(preview));
    container->addChild(std::move(body));

    // Rulebook 9.14 footer: hairline, h 36, count leading, key hints trailing.
    const auto keyHint = [scale](std::string text, std::string key, Label** out = nullptr) {
      return ui::row(
          {.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale},
          ui::label({.out = out, .text = std::move(text), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)}),
          ui::row(
              {.align = FlexAlign::Center,
               .paddingH = Tokens::spaceP4 * scale,
               .fill = colorSpecFromRole(ColorRole::Fill1),
               .radius = Tokens::radiusR4 * scale},
              ui::label({.text = std::move(key), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)})
          )
      );
    };
    auto footer = ui::column({.out = &m_pillFooter, .align = FlexAlign::Stretch, .visible = false, .participatesInLayout = false});
    footer->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = 1.0F}));
    auto footerRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP12 * scale,
        .paddingH = Tokens::spaceP20 * scale,
        .minHeight = Tokens::componentLauncherFooter * scale,
        .fillWidth = true,
    });
    footerRow->addChild(ui::label({
        .out = &m_pillCount,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label3),
        .flexGrow = 1.0F,
    }));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.open"), "↩", &m_pillOpenHint));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.actions-hint"), "⌘K"));
    auto kindHint = keyHint(i18n::tr("launcher.pill.kind-hint"), "⇥");
    m_pillKindHint = kindHint.get();
    footerRow->addChild(std::move(kindHint));
    auto backHint = keyHint(i18n::tr("launcher.pill.back-hint"), "⌫");
    m_pillBackHint = backHint.get();
    footerRow->addChild(std::move(backHint));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.close-hint"), "esc"));
    footer->addChild(std::move(footerRow));
    container->addChild(std::move(footer));
    setRoot(std::move(container));
    // The rulebook 9.14 sheet has no category tabs: the filter is not in the tree and dies with this scope.
    m_categoryFilter = nullptr;
  } else {
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    container->addChild(std::move(input));
    container->addChild(std::move(categoryFilter));
    container->addChild(std::move(body));
    setRoot(std::move(container));
  }

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this] { refreshLauncherAppIconColorization(); });

  syncLauncherListStyle();
}

void LauncherPanel::refreshLauncherAppIconColorization() {
  if (m_listAdapter == nullptr || m_gridAdapter == nullptr || m_grid == nullptr) {
    return;
  }
  const LauncherListStyle style = launcherListStyleFrom(m_browserMode, m_config, contentScale(), panelCardOpacity());
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setListStyle(style);
  }
  m_grid->notifyDataChanged();
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::updatePageDots(std::size_t page, std::size_t pages) {
  if (m_pageDotsRow == nullptr || m_grid == nullptr) {
    return;
  }
  const bool show = m_usingAppGrid && pages > 1;
  // Called from inside the grid's layout pass, where these changes do not mark the panel dirty.
  m_pageDotsChanged = true;
  m_pageDotsRow->setVisible(show);
  m_pageDotsRow->setParticipatesInLayout(show);
  const float s = contentScale();
  const float dot = Tokens::componentPageDotsDot * s;
  if (m_pageDots.size() != pages) {
    while (!m_pageDotsRow->children().empty()) {
      m_pageDotsRow->removeChild(m_pageDotsRow->children().back().get());
    }
    m_pageDots.clear();
    for (std::size_t i = 0; i < pages; ++i) {
      auto pill = std::make_unique<Flex>();
      pill->setMinHeight(dot);
      pill->setRadius(dot * 0.5F);
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setHitTestOutset({.left = dot * 0.5F, .top = dot, .right = dot * 0.5F, .bottom = dot});
      area->setOnClick([this, i](const InputArea::PointerData&) { m_grid->scrollToPage(i); });
      m_pageDots.push_back(pill.get());
      pill->addChild(std::move(area));
      m_pageDotsRow->addChild(std::move(pill));
    }
  }
  for (std::size_t i = 0; i < m_pageDots.size(); ++i) {
    const bool current = i == page;
    const float width = current ? Tokens::componentPageDotsCurrentWidth * s : dot;
    m_pageDots[i]->setMinWidth(width);
    m_pageDots[i]->setMaxWidth(width);
    m_pageDots[i]->setFill(colorSpecFromRole(current ? ColorRole::Label1 : ColorRole::Label3));
    auto* area = static_cast<InputArea*>(m_pageDots[i]->children().front().get());
    area->setFrameSize(width, dot);
  }
}

bool LauncherPanel::shouldUseAppGrid() const {
  if (m_browserMode) {
    return isAppsQuery(m_query) && !m_results.empty() && supportsResultGrid();
  }
  return m_config != nullptr && m_config->config().shell.launcher.appGrid && !m_results.empty() && supportsResultGrid();
}

bool LauncherPanel::supportsResultGrid() const {
  if (!m_launcherShowIcons || shouldUseDetailPresentation()) {
    return false;
  }
  if (m_showingProviderOverview) {
    return true;
  }
  const auto providerSupportsGrid = [this](std::string_view id) {
    const auto provider = std::ranges::find(m_providers, id, [](const auto& candidate) { return candidate->id(); });
    return provider != m_providers.end() && (*provider)->supportsGrid();
  };
  if (!m_resultProviderId.empty()) {
    return providerSupportsGrid(m_resultProviderId);
  }
  return std::ranges::all_of(m_results, [&](const LauncherResult& result) {
    return providerSupportsGrid(result.providerId);
  });
}

void LauncherPanel::queueResultsTransition(ResultsTransition transition) {
  if (!m_browserMode || transition == ResultsTransition::None) {
    return;
  }
  m_pendingResultsTransition = transition;
}

void LauncherPanel::animateResultsTransition() {
  const ResultsTransition transition = std::exchange(m_pendingResultsTransition, ResultsTransition::None);
  if (transition == ResultsTransition::None || m_resultsViewport == nullptr) {
    return;
  }

  AnimationManager* animations = m_resultsViewport->animationManager();
  if (animations == nullptr) {
    m_resultsViewport->setOpacity(1.0F);
    m_resultsViewport->setScale(1.0F);
    m_resultsTransitionActive = false;
    return;
  }

  animations->cancelForOwner(m_resultsViewport);
  if (m_resultsTransitionActive) {
    m_resultsViewport->setPosition(m_resultsTransitionBaseX, m_resultsViewport->y());
  }
  constexpr float startOpacity = 0.78F;
  Flex* const viewport = m_resultsViewport;
  const float visualDirection = transition == ResultsTransition::NavigationForward ? 1.0F : -1.0F;
  const float direction = Style::rtl() ? -visualDirection : visualDirection;
  const float travel = std::min(viewport->width() * 0.12F, 72.0F * contentScale());
  m_resultsTransitionBaseX = viewport->x();
  m_resultsTransitionActive = true;
  viewport->setOpacity(startOpacity);
  viewport->setScale(1.0F);
  viewport->setPosition(m_resultsTransitionBaseX + direction * travel, viewport->y());
  animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springPage),
      [this, viewport, direction, travel](float progress) {
        viewport->setOpacity(startOpacity + (1.0F - startOpacity) * progress);
        viewport->setPosition(m_resultsTransitionBaseX + direction * travel * (1.0F - progress), viewport->y());
      },
      [this, viewport] {
        viewport->setOpacity(1.0F);
        viewport->setScale(1.0F);
        viewport->setPosition(m_resultsTransitionBaseX, viewport->y());
        m_resultsTransitionActive = false;
      },
      viewport
  , 0.0F, Unit::Progress);
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  if (m_categoryFilterRebuildPending) {
    rebuildCategoryFilter();
  }
  updateLauncherOptionButtons();
  syncLauncherListStyle();
  syncLauncherViewLayout(&renderer);
  if (m_browserMode) {
    bindPreview(renderer);
  }

  m_container->setSize(width, height);
  m_container->layout(renderer);
  const float previousRowHeight = m_launcherRowHeight;
  updateLauncherGridMetrics(renderer);
  if (std::abs(previousRowHeight - m_launcherRowHeight) >= 0.5F || m_pageDotsChanged) {
    m_pageDotsChanged = false;
    m_container->layout(renderer);
  }
}

void LauncherPanel::doUpdate(Renderer& renderer) {
  if (m_browserMode) {
    syncLauncherAvatar(renderer);
  }
}

void LauncherPanel::syncLauncherAvatar(Renderer& renderer) {
  if (m_launcherAvatar == nullptr || m_launcherAvatarFallback == nullptr || m_config == nullptr) {
    return;
  }

  const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
  if (displayPath.empty()) {
    m_launcherAvatarPath.clear();
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
    return;
  }

  if (displayPath != m_launcherAvatarPath) {
    m_launcherAvatarPath = displayPath;
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
  }

  const int targetSize = static_cast<int>(std::round(kLauncherAvatarSize * contentScale()));
  const bool ready = m_asyncTextures != nullptr
      ? m_launcherAvatar->setSourceFileAsync(renderer, *m_asyncTextures, displayPath, targetSize, true)
      : m_launcherAvatar->setSourceFile(renderer, displayPath, targetSize, true);
  if (ready) {
    m_launcherAvatar->setVisible(true);
    m_launcherAvatar->setParticipatesInLayout(true);
    m_launcherAvatarFallback->setVisible(false);
    m_launcherAvatarFallback->setParticipatesInLayout(false);
  }
}

std::string LauncherPanel::handoffContext() const {
  return m_modeProviderId.empty() ? m_query : std::string(kModeContextPrefix) + m_modeProviderId;
}

void LauncherPanel::onOpen(std::string_view context) {
  for (auto& provider : m_providers) {
    applyProviderConfig(*provider);
    provider->onLauncherOpened();
  }

  // Pick up apps installed since the last scan (notably Nix profile swaps that
  // inotify cannot observe). Cheap stat-only check; only rescans on real change.
  refreshDesktopEntriesIfSourcesChanged();

  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = true;
  m_applyDefaultViewPending = m_browserMode;
  m_showingProviderOverview = false;
  m_launcherProviderViewId.clear();
  m_sessionReturnView.reset();

  // "mode:<provider>" or a mode provider's prefix (Mod+Period sends "/emo") opens that mode with an empty field.
  std::string initialValue(context);
  m_modeProviderId.clear();
  if (m_browserMode) {
    std::string_view requested;
    const bool modeContext = initialValue.starts_with(kModeContextPrefix);
    if (modeContext) {
      requested = std::string_view(initialValue).substr(kModeContextPrefix.size());
    } else {
      for (const std::string_view id : kModeProviders) {
        const auto provider = std::ranges::find(m_providers, id, [](const auto& p) { return p->id(); });
        if (provider != m_providers.end() && !(*provider)->prefix().empty()
            && StringUtils::trim(initialValue) == (*provider)->prefix()) {
          requested = id;
        }
      }
    }
    const auto known = std::ranges::find(kModeProviders, requested);
    if (!requested.empty() && known != kModeProviders.end()
        && std::ranges::any_of(m_providers, [&](const auto& p) { return p->id() == requested; })) {
      m_modeProviderId = std::string(requested);
    }
    if (modeContext || !m_modeProviderId.empty()) {
      initialValue.clear(); // an unknown mode opens plain search, never a search for "mode:..."
    }
    syncModeTabs();
  }
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
    m_input->setValue(singleLinePreview(initialValue));
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(initialValue);
}

void LauncherPanel::onClose() {
  stopPreviewGif();
  m_sentLiquid = -1.0F;
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures] { asyncTextures->trimUnused(0); });
  }

  for (auto& provider : m_providers) {
    provider->reset();
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  m_filesScope = false;
  m_kindIndex = 0;
  m_packIndex = 0;
  if (auto* stickers = stickersProvider()) {
    stickers->setPack({});
  }
  if (auto* files = filesProvider()) {
    files->setScopeKind({});
  }
  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = false;
  m_applyDefaultViewPending = false;
  m_syncingLauncherNavigation = false;
  m_showingProviderOverview = false;
  m_launcherProviderViewId.clear();
  m_sessionReturnView.reset();
  m_pendingResultsTransition = ResultsTransition::None;
  m_resultsTransitionActive = false;
  m_resultsTransitionBaseX = 0.0F;
  m_selectedIndex = 0;
  m_usingAppGrid = false;
  m_launcherRowHeight = 0.0F;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gridAdapter.reset();
  m_launcherSectionsAdapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_pillSearchGlyph = nullptr;
  m_categoryFilter = nullptr;
  m_body = nullptr;
  m_resultsViewport = nullptr;
  m_launcherToolbar = nullptr;
  m_launcherNavigation = nullptr;
  m_launcherAvatar = nullptr;
  m_launcherAvatarFallback = nullptr;
  m_launcherSettingsButton = nullptr;
  m_launcherUsageButton = nullptr;
  m_launcherGridButton = nullptr;
  m_launcherPowerButton = nullptr;
  m_grid = nullptr;
  m_pageDotsRow = nullptr;
  m_pageDots.clear();
  m_launcherSections = nullptr;
  m_detailScroll = nullptr;
  m_detailSubtitle = nullptr;
  m_detailBody = nullptr;
  m_emptyLabel = nullptr;
  m_launcherAvatarPath.clear();
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() { reapplyCurrentQuery(); }

void LauncherPanel::clearUsage() {
  m_usageTracker.clear();
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::syncUsageOrderingState() {
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::reapplyCurrentQuery() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerId;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerId == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

void LauncherPanel::setQuery(const std::string& query) {
  if (m_input == nullptr) {
    return;
  }
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_sessionReturnView.reset();
  }
  m_launcherProviderViewId.clear();
  m_input->setValue(singleLinePreview(query));
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(query);
}

void LauncherPanel::onProviderResultsChanged(const LauncherProvider& provider) {
  // Only re-gather while the panel is open and built; after onClose the scene
  // nodes are gone and a refresh would touch null grid/label pointers.
  if (m_input == nullptr || !m_searchBroker.acceptsAnswer(provider)) {
    return;
  }
  reapplyCurrentQuery();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool LauncherPanel::ignorePointerEvent() const {
  return m_config != nullptr && m_config->config().shell.launcher.ignoreMouse;
}

void LauncherPanel::onInputChanged(const std::string& text) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    m_iconResolver.invalidateMissingCache();
    m_desktopEntriesVersion = desktopVersion;
  }

  m_query = text;
  m_resultProviderId.clear();
  m_showingProviderOverview = false;
  if (m_browserMode && !text.empty() && m_activeCategoryType != All) {
    m_activeCategoryType = All;
    const auto allSlot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
    m_activeCategorySlotIndex =
        allSlot == m_categoryFilterSlots.end() ? 0 : static_cast<std::size_t>(allSlot - m_categoryFilterSlots.begin());
  }
  static const ShellConfig::LauncherConfig kDefaultLauncher{};
  const auto& launcher = m_config != nullptr ? m_config->config().shell.launcher : kDefaultLauncher;
  const bool trackUsage = m_config == nullptr || launcher.trackUsage;
  m_searchBroker.setOverviewPrefix(m_config != nullptr ? launcher.providerPrefix : "/");
  auto outcome = m_searchBroker.search({
      .text = m_browserMode && isAppsQuery(text) ? std::string() : text,
      .scopedProviderId = m_scopedProviderId,
      .providerViewId = m_filesScope ? std::string("Files")
          : !m_modeProviderId.empty()
          ? m_modeProviderId
          : (m_scopedProviderId.empty() ? m_launcherProviderViewId : std::string()),
      .applicationsOnly = m_browserMode && m_modeProviderId.empty() && (text.empty() || isAppsQuery(text)),
      .trackUsage = trackUsage,
      .sortByUsage = trackUsage && m_config != nullptr && launcher.sortByUsage,
  });
  if (outcome.providerViewMissing) {
    m_launcherProviderViewId.clear();
  }
  m_allResults = std::move(outcome.results);
  m_resultProviderId = std::move(outcome.resultProviderId);
  std::vector<LauncherCategory> newCategories = std::move(outcome.categories);
  bool hasRecentlyUsed = outcome.hasRecentlyUsed;
  const bool anyProviderLoading = outcome.loading;
  if (outcome.providerOverview) {
    m_allResults = providerOverviewResults(text);
    m_showingProviderOverview = true;
  }

  if (m_browserMode) {
    const auto applications =
        std::ranges::find(m_providers, kApplicationsProviderId, [](const auto& provider) { return provider->id(); });
    if (applications != m_providers.end()) {
      newCategories = (*applications)->categories();
      hasRecentlyUsed = trackUsage && m_usageTracker.getRecentlyUsedCount((*applications)->id()) > 0;
    }
  }

  const int iconTargetSize = static_cast<int>(
      std::round(launcherIconSize(launcherListStyleFrom(m_browserMode, m_config, contentScale(), panelCardOpacity())))
  );
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  updatePinnedApplicationState();
  const bool hasPinnedApplications = m_config != nullptr
      && !shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned).empty();

  bool categoriesChanged =
      (m_browserMode && m_categoryFilterSlots.empty()) || newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }

  if (hasRecentlyUsed != m_hasRecentlyUsed) {
    m_hasRecentlyUsed = hasRecentlyUsed;
    categoriesChanged = true;
  }
  if (m_browserMode && hasPinnedApplications != m_hasPinnedApplications) {
    m_hasPinnedApplications = hasPinnedApplications;
    categoriesChanged = true;
  }

  if (categoriesChanged) {
    updateCategoryFilterModel(newCategories);
  }

  if (m_applyDefaultViewPending) {
    applyDefaultView();
  }

  m_anyProviderLoading = anyProviderLoading;

  applyActiveCategory();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  if (m_browserMode && m_modeProviderId.empty() && m_results.empty() && !m_query.empty()) {
    // Rulebook 9.14 / docs/search.md §1: never an empty sheet, also while a slow provider answers; offer the web
    // search row.
    const auto web = std::ranges::find(m_providers, "Web", [](const auto& provider) { return provider->id(); });
    if (web != m_providers.end()) {
      for (auto& result : (*web)->query(m_query)) {
        result.providerId = (*web)->id();
        m_results.push_back(std::move(result));
      }
    }
  }
  syncLauncherViewLayout(nullptr);
  rebuildLauncherSections();
  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0F);
    if (m_launcherSections != nullptr) {
      m_launcherSections->scrollView().setScrollOffset(0.0F);
    }
  } else {
    selectResult(std::min(m_selectedIndex, m_results.size() - 1), false);
  }
  bindDetailResult();
  applyEmptyState();
  animateResultsTransition();
}

void LauncherPanel::selectResult(std::size_t index, bool scrollToSelection) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  if (shouldUseLauncherSections() && m_launcherSectionsAdapter != nullptr && m_launcherSections != nullptr) {
    m_grid->setSelectedIndex(std::nullopt);
    m_launcherSectionsAdapter->setSelectedResultIndex(index);
    m_launcherSections->notifyDataChanged();
    if (scrollToSelection) {
      if (const auto row = m_launcherSectionsAdapter->rowForResult(index)) {
        m_launcherSections->scrollToIndex(*row);
      }
    }
    return;
  }
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(index);
  }
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  const bool detail = !empty && shouldUseDetailPresentation();
  const bool sections = !empty && !detail && shouldUseLauncherSections();
  m_grid->setVisible(!empty && !detail && !sections);
  m_grid->setParticipatesInLayout(!empty && !detail && !sections);
  if (m_launcherSections != nullptr) {
    m_launcherSections->setVisible(sections);
    m_launcherSections->setParticipatesInLayout(sections);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setVisible(detail);
    m_detailScroll->setParticipatesInLayout(detail);
  }
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  syncEmojiStrip();
  syncPackStrip();
  if (m_browserMode && m_body != nullptr && m_pillSeparator != nullptr && m_pillFooter != nullptr) {
    const bool expanded = pillExpanded();
    m_body->setVisible(expanded);
    m_body->setParticipatesInLayout(expanded);
    m_pillSeparator->setVisible(expanded);
    m_pillSeparator->setParticipatesInLayout(expanded);
    const bool footer = expanded && !shouldUseAppGrid();
    m_pillFooter->setVisible(footer);
    m_pillFooter->setParticipatesInLayout(footer);
    m_pillCount->setText(
        m_filesScope ? i18n::trp("launcher.files.count", static_cast<long>(m_results.size()))
                     : i18n::tr("launcher.pill.result-count", "count",
                                std::ranges::count_if(m_results, [](const LauncherResult& r) { return r.id != FilesProvider::kShowAllId; }))
    );
    m_pillBackHint->setVisible(m_filesScope);
    m_pillBackHint->setParticipatesInLayout(m_filesScope);
    m_pillKindHint->setVisible(m_filesScope);
    m_pillKindHint->setParticipatesInLayout(m_filesScope);
    syncKindTabs();
    if (m_filesToken != nullptr) {
      if (auto* area = dynamic_cast<InputArea*>(m_filesToken->children().back().get())) {
        area->setFrameSize(m_filesToken->width(), m_filesToken->height());
      }
    }
    const bool pasteMode = !m_modeProviderId.empty();
    m_pillOpenHint->setText(i18n::tr(pasteMode ? "launcher.pill.paste" : "launcher.pill.open"));
  }
  if (empty) {
    if (m_anyProviderLoading && !m_query.empty()) {
      m_emptyLabel->setText(i18n::tr("launcher.empty.loading"));
    } else {
      m_emptyLabel->setText(
          m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
      );
    }
  }
}
