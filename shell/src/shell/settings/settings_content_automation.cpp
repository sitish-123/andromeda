#include "shell/settings/settings_content_automation.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"

#include <algorithm>
#include <array>

namespace settings {

  using namespace page_rows;

  std::string automationEventText(std::size_t hook) {
    return i18n::tr("settings.automation.when." + i18n::keySegment(std::string(kHookKinds[hook].key)));
  }

  std::string_view automationEventGlyph(std::size_t hook) {
    constexpr std::array<std::string_view, static_cast<std::size_t>(HookKind::Count)> kGlyphs{
        "rocket", "photo",   "palette",   "sun-moon",      "lock",          "lock-open",
        "logout", "refresh", "power",     "wifi",          "wifi-off",      "bluetooth",
        "bluetooth-off", "battery-charging", "battery-2", "plug", "battery", "bolt",
    };
    return kGlyphs[static_cast<std::size_t>(kHookKinds[hook].value)];
  }

  void addSettingsAutomation(Flex& content, const SettingsAutomationContext& ctx) {
    const float scale = ctx.scale;
    Flex* card = addSettingsGroupCard(content, i18n::tr("settings.automation.rules"), scale);
    bool any = false;
    if (ctx.hooks != nullptr) {
      for (std::size_t hook = 0; hook < std::size(kHookKinds); ++hook) {
        const auto kind = static_cast<std::size_t>(kHookKinds[hook].value);
        const auto& commands = ctx.hooks->commands[kind];
        for (std::size_t i = 0; i < commands.size(); ++i) {
          any = true;
          Flex* r = addTwoLineRow(*card, automationEventGlyph(hook), automationEventText(hook),
                                  i18n::tr("settings.automation.run", "command", commands[i]), scale);
          r->addChild(ui::toggle({
              .checked = !std::ranges::contains(ctx.hooks->disabled[kind], commands[i]),
              .scale = scale,
              .onChange = [set = ctx.setEnabled, hook, command = commands[i]](bool on) { set(hook, command, on); },
          }));
          r->addChild(chevronButton(automationEventText(hook), scale,
                                    [open = ctx.openRule, hook, i] { open(hook, i); }));
        }
      }
    }
    addActionRow(*card, i18n::tr("settings.automation.add"), any, scale,
                 [open = ctx.openRule] { open(std::nullopt, std::nullopt); });
    addSettingsRowSeparators(*card, scale);
    addFootnote(*card, i18n::tr("settings.automation.footnote"), scale);
  }

} // namespace settings
