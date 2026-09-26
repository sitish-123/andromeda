#include "shell/settings/settings_content_language.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <optional>

namespace settings {

  namespace {

    constexpr float kSelectWidth = 260.0F;

    std::unique_ptr<Flex> makeRow(std::string_view title, std::unique_ptr<Node> control, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      row->addChild(ui::spacer());
      row->addChild(std::move(control));
      return row;
    }

    std::string currentLang(const LocaleService& locale) {
      for (const std::string& assignment : locale.locale()) {
        if (assignment.starts_with("LANG=")) {
          return assignment.substr(5);
        }
      }
      return {};
    }

  } // namespace

  void addSettingsLanguage(Flex& content, const SettingsLanguageContext& ctx) {
    if (ctx.locale == nullptr || !ctx.locale->ready() || ctx.catalog == nullptr) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.language.unavailable"), ctx.scale));
      return;
    }
    const LocaleService& locale = *ctx.locale;
    Flex* body = addSettingsCard(content, i18n::tr("settings.language.title"), ctx.scale);

    const std::vector<std::string> langs = LocaleService::supportedLocales();
    const std::string lang = currentLang(locale);
    const auto langIt = std::ranges::find(langs, lang);
    body->addChild(makeRow(
        i18n::tr("settings.language.language"),
        ui::select({
            .options = langs,
            .selectedIndex = langIt != langs.end() ? std::optional<std::size_t>(langIt - langs.begin()) : std::nullopt,
            .fontSize = Style::fontSizeBody * ctx.scale,
            .controlHeight = Style::controlHeight * ctx.scale,
            .glyphSize = Style::fontSizeBody * ctx.scale,
            .width = kSelectWidth * ctx.scale,
            .height = Style::controlHeight * ctx.scale,
            .onSelectionChanged = [set = ctx.setLang,
                                   langs](std::size_t index, std::string_view) { set(langs[index]); },
        }),
        ctx.scale
    ));

    std::vector<std::string> layoutLabels;
    std::vector<std::string> layoutNames;
    std::optional<std::size_t> layoutSelected;
    for (const xkb::Layout& layout : ctx.catalog->layouts) {
      if (layout.name == locale.x11Layout()) {
        layoutSelected = layoutNames.size();
      }
      layoutNames.push_back(layout.name);
      layoutLabels.push_back(layout.description);
    }
    body->addChild(makeRow(
        i18n::tr("settings.language.keyboard-layout"),
        ui::select({
            .options = std::move(layoutLabels),
            .selectedIndex = layoutSelected,
            .fontSize = Style::fontSizeBody * ctx.scale,
            .controlHeight = Style::controlHeight * ctx.scale,
            .glyphSize = Style::fontSizeBody * ctx.scale,
            .width = kSelectWidth * ctx.scale,
            .height = Style::controlHeight * ctx.scale,
            .onSelectionChanged = [set = ctx.setX11Layout, layoutNames = std::move(layoutNames)](
                                      std::size_t index, std::string_view
                                  ) { set(layoutNames[index], ""); },
        }),
        ctx.scale
    ));

    if (!locale.lastError().empty()) {
      body->addChild(
          makeLabel(locale.lastError(), Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error))
      );
    }
  }

} // namespace settings
