#include "shell/settings/settings_content_services.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"
#include "util/string_utils.h"

namespace settings {

  using namespace page_rows;

  void addSettingsServices(Flex& content, const SettingsServicesContext& ctx) {
    const float scale = ctx.scale;

    Flex* media = addSettingsGroupCard(content, i18n::tr("settings.services-page.media"), scale);
    if (ctx.players.empty()) {
      addInfoRow(*media, "", i18n::tr(ctx.playersLoaded ? "settings.services-page.players-none"
                                                        : "settings.services-page.loading"),
                 {}, scale);
    }
    for (const auto& player : ctx.players) {
      addSwitchRow(*media, "player-play", player.label, player.shown, scale, player.setShown);
    }
    addSettingsRowSeparators(*media, scale);
    addFootnote(*media, i18n::tr("settings.services-page.media-footnote"), scale);

    Flex* background = addSettingsGroupCard(content, i18n::tr("settings.services-page.background"), scale);
    if (!ctx.backgroundLoaded || ctx.background.empty()) {
      auto r = row(scale);
      r->addChild(title(i18n::tr(ctx.backgroundLoaded ? "settings.services-page.background-none"
                                                      : "settings.services-page.loading"),
                        scale));
      background->addChild(std::move(r));
    }
    for (const auto& app : ctx.background) {
      auto r = row(scale);
      r->setGap(Style::spaceMd * scale);
      auto icon = ctx.appIcon ? ctx.appIcon(app.icon, Tokens::sizeSymbolL * scale) : nullptr;
      r->addChild(icon != nullptr ? std::move(icon) : symbol("app-window", Tokens::sizeSymbol * scale, ColorRole::Label2));
      r->addChild(title(app.label, scale));
      if (app.startsAtLogin) {
        r->addChild(valueText(i18n::tr("settings.services-page.starts-at-login"), scale));
      }
      r->addChild(ui::toggle({.checked = app.allowed, .scale = scale,
                              .onChange = [set = ctx.setBackground, id = app.id](bool on) { set(id, on); }}));
      background->addChild(std::move(r));
    }
    addSettingsRowSeparators(*background, scale);
    addFootnote(*background, i18n::tr("settings.services-page.background-footnote"), scale);

    if (!ctx.defaults.empty()) {
      Flex* defaults = addSettingsGroupCard(content, i18n::tr("settings.pages.default-apps"), scale);
      for (const auto& app : ctx.defaults) {
        addInfoRow(*defaults, "", app.label, app.app.empty() ? i18n::tr("settings.services-page.none") : app.app, scale,
                   ctx.openDefaultApps);
      }
      addSettingsRowSeparators(*defaults, scale);
    }
  }

} // namespace settings
