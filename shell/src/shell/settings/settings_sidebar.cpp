#include "shell/settings/settings_sidebar.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/settings_pages.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    constexpr float kChildPaddingLeft = metrics::kSidebarRowPaddingH + metrics::kSidebarTile + metrics::kSidebarRowGap;

    void addNavButton(RovingListNavHost& nav, std::unique_ptr<Button> button, std::function<void()> onClick) {
      Button* raw = button.get();
      nav.registerItem(raw, std::move(onClick));
      nav.addChild(std::move(button));
    }

    std::string normalizedConfigId(std::string_view text) { return StringUtils::trim(text); }

    bool isValidConfigId(std::string_view text) {
      const auto trimmed = StringUtils::trim(text);
      if (trimmed.empty()) {
        return false;
      }
      return std::ranges::all_of(trimmed, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-'; });
    }

    bool barNameExists(const std::vector<std::string>& barNames, std::string_view name) {
      return std::ranges::contains(barNames, name);
    }

    std::string nextAvailableBarName(const std::vector<std::string>& barNames) {
      for (std::size_t i = 1;; ++i) {
        const std::string candidate = i == 1 ? "bar" : std::format("bar_{}", i);
        if (!barNameExists(barNames, candidate)) {
          return candidate;
        }
      }
    }

    std::unique_ptr<Flex> makeTile(std::string_view glyph, float scale, bool selected) {
      return ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = selected ? selectionStyle().tileFill : frameColors().tileFill,
           .radius = Style::scaledRadiusSm(scale),
           .minWidth = metrics::kSidebarTile * scale,
           .minHeight = metrics::kSidebarTile * scale,
           .width = metrics::kSidebarTile * scale,
           .height = metrics::kSidebarTile * scale},
          ui::glyph({
              .glyph = std::string(glyph),
              .glyphSize = metrics::kSidebarTileGlyph * scale,
              .color = selected ? selectionStyle().tileGlyph : frameColors().tileGlyph,
          })
      );
    }

    // A page row: icon tile, label, accent fill when it is the open page.
    std::unique_ptr<Button> makePageRow(
        std::string_view glyph, std::string text, float scale, bool selected, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .customPalette = selected ? std::optional{selectedRowPalette(selectionStyle().rowFill, selectionStyle().rowLabel)}
                                    : std::nullopt,
          .minHeight = metrics::kSidebarRowHeight * scale,
          .paddingV = 0.0F,
          .paddingH = metrics::kSidebarRowPaddingH * scale,
          .gap = metrics::kSidebarRowGap * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [glyph = std::string(glyph), scale, selected](Button& button) {
            button.insertChildAt(0, makeTile(glyph, scale, selected));
            button.setTabStop(false);
          },
      });
    }

    // Secondary sidebar nav style: indented compact rows for bars and monitors.
    std::unique_ptr<Button> makeSecondaryNavButton(
        std::string_view glyph, std::string text, float scale, bool selected, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = std::string(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeCaption * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .customPalette = selected
              ? std::optional{selectedRowPalette(selectionStyle().childFill, colorSpecFromRole(ColorRole::Label1))}
              : std::nullopt,
          .minHeight = Style::controlHeight * scale,
          .paddingTop = Style::spaceXs * scale,
          .paddingRight = Style::spaceMd * scale,
          .paddingBottom = Style::spaceXs * scale,
          .paddingLeft = kChildPaddingLeft * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& button) { button.setTabStop(false); },
      });
    }

    std::unique_ptr<Button> makeCreateButton(std::string text, float scale, std::function<void()> onClick) {
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Button> makeCreateCancelButton(float scale, std::function<void()> onClick) {
      return ui::button({
          .glyph = "close",
          .glyphSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Button> makeAccountCard(SettingsSidebarContext& ctx, std::function<void()> onClick) {
      const float scale = ctx.scale;
      const float avatarSize = metrics::kAccountAvatar * scale;
      std::unique_ptr<Node> avatar;
      if (ctx.renderer != nullptr && ctx.avatarTextures != nullptr && !ctx.accountAvatarPath.empty()) {
        Image* image = nullptr;
        auto node = ui::image({
            .out = &image,
            .fit = ImageFit::Cover,
            .radius = avatarSize * 0.5F,
            .width = avatarSize,
            .height = avatarSize,
        });
        if (image != nullptr
            && image->setSourceFileAsync(
                *ctx.renderer, *ctx.avatarTextures, ctx.accountAvatarPath, static_cast<int>(avatarSize), false
            )) {
          avatar = std::move(node);
        }
      }
      if (avatar == nullptr) {
        const std::string initial = StringUtils::truncateUtf8CodePoints(ctx.accountName, 1);
        avatar = ui::row(
            {.align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .fill = frameColors().avatarFill,
             .radius = avatarSize * 0.5F,
             .minWidth = avatarSize,
             .minHeight = avatarSize,
             .width = avatarSize,
             .height = avatarSize},
            ui::label({
                .text = initial,
                .fontSize = Style::fontSizeTitle * scale,
                .fontWeight = FontWeight::Bold,
                .color = frameColors().avatarInitial,
            })
        );
      }
      auto button = ui::button({
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .minHeight = (metrics::kAccountAvatar + 2.0F * Style::spaceSm) * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = metrics::kSidebarRowPaddingH * scale,
          .gap = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& card) { card.setTabStop(false); },
      });
      button->addChild(std::move(avatar));
      button->addChild(
          ui::column(
              {.align = FlexAlign::Start, .justify = FlexJustify::Center},
              ui::label({
                  .text = ctx.accountName,
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::Label1),
              }),
              ui::label({
                  .text = i18n::tr("settings.account.caption"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::Label2),
              })
          )
      );
      return button;
    }

  } // namespace

  std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx) {
    const std::vector<std::string> existingBarNames = ctx.availableBars;
    const std::string nextBarName = nextAvailableBarName(existingBarNames);

    auto* creatingBarName = &ctx.creatingBarName;
    const auto clearTransientState = std::move(ctx.clearTransientState);
    const auto requestRebuild = std::move(ctx.requestRebuild);
    const auto createBar = std::move(ctx.createBar);
    const auto navigate = std::move(ctx.navigate);
    const float scale = ctx.scale;
    const bool showActiveTab = !ctx.globalSearchActive;

    auto sidebarScroll = ui::scrollView({
        .state = &ctx.sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fill = clearColorSpec(),
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });
    if (ctx.outScroll != nullptr) {
      *ctx.outScroll = sidebarScroll.get();
    }

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::Roving,
        .keepItemsInTabOrder = false,
        .scrollIntoView = std::move(ctx.scrollSidebarNodeIntoView),
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("settings.sidebar");
    sidebarNav->setGap(metrics::kSidebarGap * scale);
    sidebarNav->setPadding(0.0F, metrics::kSidebarPaddingH * scale, metrics::kSidebarPaddingV * scale, metrics::kSidebarPaddingH * scale);
    RovingListNavHost* nav = sidebarNav.get();

    const auto onAccountClick = ctx.openAccount;
    addNavButton(*nav, makeAccountCard(ctx, onAccountClick), onAccountClick);

    std::optional<PageGroup> lastGroup;
    for (const auto* page : ctx.pages) {
      if (lastGroup.has_value() && *lastGroup != page->group) {
        nav->addChild(ui::column({.height = metrics::kSidebarGroupGap * scale}));
      }
      lastGroup = page->group;

      const std::string pageId(page->id);
      const bool selected = showActiveTab && pageId == ctx.selectedPage;
      const auto onClick = [navigate, pageId]() { navigate(pageId, {}); };
      addNavButton(
          *nav, makePageRow(page->glyph, i18n::tr(settingsPageLabelKey(*page)), scale, selected, onClick), onClick
      );

      if (pageId != "bar" || !selected) {
        continue;
      }
      for (const auto& barName : ctx.availableBars) {
        const bool barSelected = showActiveTab && ctx.selectedBarName == barName;
        const auto onBarClick = [navigate, barName]() { navigate("bar", barName); };
        addNavButton(
            *nav,
            makeSecondaryNavButton(
                "crop-3-2", i18n::tr("settings.entities.bar.label", "name", barName), scale, barSelected, onBarClick
            ),
            onBarClick
        );
      }
      const auto onNewBarClick = [creatingBarName, nextBarName, clearTransientState, requestRebuild]() {
        clearTransientState();
        *creatingBarName = nextBarName;
        requestRebuild();
      };
      auto newBar = makeSecondaryNavButton("add", i18n::tr("settings.entities.bar.new"), scale, false, onNewBarClick);
      newBar->setVariant(ButtonVariant::Ghost);
      addNavButton(*nav, std::move(newBar), onNewBarClick);
    }

    if (!creatingBarName->empty()) {
      auto createPanel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .configure = [scale](Flex& panel) { panel.setPadding(0.0F, Style::spaceXs * scale); },
      });

      Input* inputPtr = nullptr;
      auto input = ui::input({
          .out = &inputPtr,
          .value = *creatingBarName,
          .placeholder = i18n::tr("settings.entities.bar.id-placeholder"),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceXs * scale,
          .width = metrics::kNewBarInputWidth * scale,
          .height = Style::controlHeight * scale,
      });

      auto doCreate = [existingBarNames, createBar, inputPtr](const std::string& rawName) {
        const std::string name = normalizedConfigId(rawName);
        if (!isValidConfigId(name) || barNameExists(existingBarNames, name)) {
          inputPtr->setInvalid(true);
          return;
        }
        inputPtr->setInvalid(false);
        createBar(name);
      };

      inputPtr->setOnChange([creatingBarName, inputPtr](const std::string& value) {
        *creatingBarName = value;
        inputPtr->setInvalid(false);
      });
      inputPtr->setOnSubmit([doCreate](const std::string& text) mutable { doCreate(text); });

      createPanel->addChild(std::move(input));
      createPanel->addChild(
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceXs * scale,
              },
              makeCreateButton(
                  i18n::tr("settings.entities.bar.create"), scale,
                  [doCreate, inputPtr] mutable { doCreate(inputPtr->value()); }
              ),
              makeCreateCancelButton(scale, [creatingBarName, requestRebuild] {
                creatingBarName->clear();
                requestRebuild();
              })
          )
      );
      sidebarNav->addChild(std::move(createPanel));
    }


    auto* navHost = sidebarScroll->content();
    navHost->setDirection(FlexDirection::Vertical);
    navHost->setAlign(FlexAlign::Stretch);
    navHost->addChild(std::move(sidebarNav));

    if (ctx.outNav != nullptr) {
      *ctx.outNav = nav;
    }

    auto sidebar = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .minWidth = metrics::kSidebarWidth * scale,
        .fillHeight = true,
        .width = metrics::kSidebarWidth * scale,
        .configure = [scale](Flex& column) {
          column.setPadding(metrics::kSidebarPaddingV * scale, 0.0F, 0.0F, 0.0F);
        },
    });
    if (ctx.searchField != nullptr) {
      auto searchHost = ui::column({.align = FlexAlign::Stretch});
      searchHost->setPadding(0.0F, metrics::kSidebarPaddingH * scale);
      searchHost->addChild(std::move(ctx.searchField));
      sidebar->addChild(std::move(searchHost));
    }
    sidebar->addChild(std::move(sidebarScroll));
    return sidebar;
  }

} // namespace settings
