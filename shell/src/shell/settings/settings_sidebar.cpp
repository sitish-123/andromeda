#include "shell/settings/settings_sidebar.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    constexpr float kSidebarWidth = 200.0F;
    constexpr float kSidebarPadding = 6.0F;
    constexpr float kSidebarGap = 2.0F;
    constexpr float kPrimaryNavGlyphSize = 18.0F;
    constexpr float kPrimaryNavGap = 6.0F;
    constexpr float kPrimaryNavPaddingH = 10.0F;

    void addNavButton(RovingListNavHost& nav, std::unique_ptr<Button> button, std::function<void()> onClick) {
      Button* raw = button.get();
      nav.registerItem(raw, onClick);
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

    void makeButtonLabelBold(Button& button) {
      if (button.label() != nullptr) {
        button.label()->setFontWeight(FontWeight::Bold);
      }
    }

    // Primary sidebar nav style: top-level section rows with a bolder label.
    std::unique_ptr<Button> makePrimaryNavButton(
        std::string_view glyph, std::string text, float scale, bool selected, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = std::string(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = kPrimaryNavGlyphSize * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = kPrimaryNavPaddingH * scale,
          .gap = kPrimaryNavGap * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& button) {
            makeButtonLabelBold(button);
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
          .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .minHeight = Style::controlHeightSm * scale,
          .paddingTop = Style::spaceXs * scale,
          .paddingRight = Style::spaceMd * scale,
          .paddingBottom = Style::spaceXs * scale,
          .paddingLeft = Style::spaceLg * scale,
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
          .minHeight = Style::controlHeightSm * scale,
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
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = std::move(onClick),
      });
    }

  } // namespace

  std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx) {
    std::vector<std::string> existingBarNames = ctx.availableBars;
    const std::string nextBarName = nextAvailableBarName(existingBarNames);

    auto* scroll = &ctx.contentScrollState;
    auto* selectedSection = &ctx.selectedSection;
    auto* selectedBarName = &ctx.selectedBarName;
    auto* selectedMonitorOverride = &ctx.selectedMonitorOverride;
    auto* creatingBarName = &ctx.creatingBarName;

    const auto clearTransientState = std::move(ctx.clearTransientState);
    const auto clearSearchQuery = std::move(ctx.clearSearchQuery);
    const auto requestRebuild = std::move(ctx.requestRebuild);
    const auto createBar = std::move(ctx.createBar);
    const float scale = ctx.scale;
    const bool searchActive = ctx.globalSearchActive;
    const bool showActiveTab = !searchActive;

    auto sidebarScroll = ui::scrollView({
        .state = &ctx.sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fill = ctx.config.shell.settingsWindowTranslucent ? clearColorSpec() : colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusXl(scale),
        .minWidth = kSidebarWidth * scale,
        .fillHeight = true,
        .width = kSidebarWidth * scale,
        .height = 0.0F,
        .configure = [](ScrollView& scrollView) { scrollView.clearBorder(); },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .scrollIntoView = std::move(ctx.scrollSidebarNodeIntoView),
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("settings.sidebar");
    sidebarNav->setGap(kSidebarGap * scale);
    sidebarNav->setPadding(kSidebarPadding * scale);
    RovingListNavHost* nav = sidebarNav.get();

    const auto selectedCategory = [&]() -> std::optional<SettingsCategory> {
      if (*selectedSection == "bar") {
        return SettingsCategory::Desktop;
      }
      const auto section = settingsSectionFromId(*selectedSection);
      return section.has_value() ? std::optional{settingsSectionCategory(*section)} : std::nullopt;
    }();

    const auto navigateTo = [selectedSection, selectedBarName, selectedMonitorOverride, scroll, searchActive,
                             clearTransientState, clearSearchQuery,
                             requestRebuild](std::string sectionId, std::string barName) {
      if (searchActive || *selectedSection != sectionId || *selectedBarName != barName) {
        scroll->offset = 0.0F;
      }
      *selectedSection = std::move(sectionId);
      *selectedBarName = std::move(barName);
      selectedMonitorOverride->clear();
      clearSearchQuery();
      clearTransientState();
      requestRebuild();
    };

    std::vector<SettingsCategory> categories;
    for (const auto section : ctx.sections) {
      if (!std::ranges::contains(categories, settingsSectionCategory(section))) {
        categories.push_back(settingsSectionCategory(section));
      }
    }
    if (!ctx.availableBars.empty() && !std::ranges::contains(categories, SettingsCategory::Desktop)) {
      categories.push_back(SettingsCategory::Desktop);
    }
    std::ranges::sort(categories);

    for (const auto category : categories) {
      const bool desktop = category == SettingsCategory::Desktop;
      std::vector<SettingsSection> children;
      std::ranges::copy_if(ctx.sections, std::back_inserter(children), [category](SettingsSection section) {
        return settingsSectionCategory(section) == category;
      });
      const bool expanded = selectedCategory == category;
      // A category holding one page is that page: no child row repeating its name.
      const bool singlePage = children.size() == 1 && (!desktop || ctx.availableBars.empty());
      const auto onCategoryClick = [navigateTo, expanded, desktop, children, bars = ctx.availableBars]() {
        if (expanded) {
          return;
        }
        if (desktop && !bars.empty()) {
          navigateTo("bar", bars.front());
        } else if (!children.empty()) {
          navigateTo(std::string(settingsSectionId(children.front())), {});
        }
      };
      addNavButton(
          *nav,
          makePrimaryNavButton(
              settingsCategoryGlyph(category),
              i18n::tr("settings.navigation.categories." + std::string(settingsCategoryId(category))), scale,
              singlePage && showActiveTab && expanded, onCategoryClick
          ),
          onCategoryClick
      );
      if (!expanded || singlePage) {
        continue;
      }

      if (desktop) {
        for (const auto& barName : ctx.availableBars) {
          const bool barSelected = showActiveTab && *selectedSection == "bar" && *selectedBarName == barName;
          const auto onBarClick = [navigateTo, barName]() { navigateTo("bar", barName); };
          addNavButton(
              *nav,
              makeSecondaryNavButton(
                  sectionGlyph(SettingsSection::Bar), i18n::tr("settings.entities.bar.label", "name", barName), scale,
                  barSelected, onBarClick
              ),
              onBarClick
          );
        }
        const auto onNewBarClick = [creatingBarName, nextBarName, clearTransientState, requestRebuild]() {
          clearTransientState();
          *creatingBarName = nextBarName;
          requestRebuild();
        };
        addNavButton(
            *nav,
            ui::button({
                .text = i18n::tr("settings.entities.bar.new"),
                .glyph = "add",
                .fontSize = Style::fontSizeCaption * scale,
                .glyphSize = Style::fontSizeCaption * scale,
                .contentAlign = ButtonContentAlign::Start,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * scale,
                .paddingTop = Style::spaceXs * scale,
                .paddingRight = Style::spaceMd * scale,
                .paddingBottom = Style::spaceXs * scale,
                .paddingLeft = Style::spaceLg * scale,
                .gap = Style::spaceXs * scale,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = onNewBarClick,
                .configure = [](Button& button) { button.setTabStop(false); },
            }),
            onNewBarClick
        );
      }

      for (const auto section : children) {
        const std::string sectionId(settingsSectionId(section));
        const bool selected = showActiveTab && sectionId == *selectedSection;
        const auto onClick = [navigateTo, sectionId]() { navigateTo(sectionId, {}); };
        addNavButton(
            *nav,
            makeSecondaryNavButton(
                sectionGlyph(section), i18n::tr(settingsSectionLabelKey(section)), scale, selected, onClick
            ),
            onClick
        );
      }
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
          .controlHeight = Style::controlHeightSm * scale,
          .horizontalPadding = Style::spaceXs * scale,
          .width = 120.0F * scale,
          .height = Style::controlHeightSm * scale,
      });

      auto doCreate = [existingBarNames, createBar, inputPtr](std::string rawName) {
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
                  [doCreate, inputPtr]() mutable { doCreate(inputPtr->value()); }
              ),
              makeCreateCancelButton(scale, [creatingBarName, requestRebuild]() {
                creatingBarName->clear();
                requestRebuild();
              })
          )
      );
      sidebarNav->addChild(std::move(createPanel));
    }

    auto* sidebar = sidebarScroll->content();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(sidebarNav));

    if (ctx.outNav != nullptr) {
      *ctx.outNav = nav;
    }

    return sidebarScroll;
  }

} // namespace settings
