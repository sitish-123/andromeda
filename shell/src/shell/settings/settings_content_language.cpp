#include "shell/settings/settings_content_language.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <langinfo.h>
#include <locale.h>
#include <map>
#include <optional>

namespace settings {

  namespace {

    using namespace page_rows;

    constexpr float kSelectWidth = 240.0F;
    constexpr auto kRegionKeys = std::to_array<std::string_view>(
        {"LC_TIME", "LC_NUMERIC", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER"}
    );

    std::string assignment(const LocaleService& locale, std::string_view key) {
      for (const std::string& a : locale.locale()) {
        if (a.size() > key.size() && a.starts_with(key) && a[key.size()] == '=') {
          return a.substr(key.size() + 1);
        }
      }
      return {};
    }

    // "English (United States)" from glibc's LC_IDENTIFICATION; the locale id when it is not installed.
    std::string localeName(const std::string& id) {
      std::string name = id;
      if (locale_t loc = newlocale(LC_IDENTIFICATION_MASK, id.c_str(), nullptr); loc != nullptr) {
        const std::string language = nl_langinfo_l(_NL_IDENTIFICATION_LANGUAGE, loc);
        const std::string territory = nl_langinfo_l(_NL_IDENTIFICATION_TERRITORY, loc);
        if (!language.empty()) {
          name = territory.empty() ? language : language + " (" + territory + ")";
        }
        freelocale(loc);
      }
      return name;
    }

    // "Sun 8 Nov 2026 · 22:08 · 1,234.56 · Rs 1,234" in the region's formats.
    std::string formatPreview(const std::string& region) {
      locale_t loc = newlocale(LC_TIME_MASK | LC_NUMERIC_MASK | LC_MONETARY_MASK, region.c_str(), nullptr);
      if (loc == nullptr) {
        return {};
      }
      const std::time_t now = std::time(nullptr);
      std::tm tm{};
      localtime_r(&now, &tm);
      char date[96]{};
      strftime_l(date, sizeof(date), "%a %e %b %Y · %X", &tm, loc);
      const std::string thousands = nl_langinfo_l(THOUSEP, loc);
      const std::string number = "1" + thousands + "234" + nl_langinfo_l(RADIXCHAR, loc) + "56";
      std::string money = "1" + std::string(nl_langinfo_l(MON_THOUSANDS_SEP, loc)) + "234";
      const std::string currency = nl_langinfo_l(CRNCYSTR, loc);
      freelocale(loc);
      if (currency.size() > 1) {
        // '-' puts the symbol first, '+' after, '.' in place of the radix point.
        const std::string symbol = currency.substr(1);
        money = currency.front() == '+' ? money + " " + symbol : symbol + " " + money;
      }
      return std::string(date) + " · " + number + " · " + money;
    }

    std::unique_ptr<Node> select(std::vector<std::string> options, std::optional<std::size_t> selected, float scale,
                                 std::function<void(std::size_t)> onChange) {
      return ui::select({
          .options = std::move(options),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Tokens::sizeControlS * scale,
          .glyphSize = Tokens::sizeSymbolInline * scale,
          .width = kSelectWidth * scale,
          .onSelectionChanged = [onChange = std::move(onChange)](std::size_t index, std::string_view) { onChange(index); },
      });
    }

  } // namespace

  std::vector<std::pair<std::string, std::string>> loadLocaleNames() {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& id : LocaleService::supportedLocales()) {
      std::string name = localeName(id);
      out.emplace_back(std::move(id), std::move(name));
    }
    return out;
  }

  void addSettingsLanguage(Flex& content, const SettingsLanguageContext& ctx) {
    const float scale = ctx.scale;
    if (ctx.locale == nullptr || !ctx.locale->ready() || ctx.catalog == nullptr) {
      Flex* card = addSettingsGroupCard(content, i18n::tr("settings.language.title"), scale);
      addInfoRow(*card, "", i18n::tr("settings.language.language"), localeName(setlocale(LC_MESSAGES, nullptr)), scale);
      addFootnote(*card, i18n::tr("settings.language.unavailable"), scale);
      return;
    }
    const LocaleService& locale = *ctx.locale;
    std::vector<std::string> langs;
    std::vector<std::string> labels;
    for (const auto& [id, name] : ctx.locales) {
      langs.push_back(id);
      labels.push_back(name);
    }
    const std::string lang = assignment(locale, "LANG");
    std::string region = assignment(locale, "LC_TIME");
    if (region.empty()) {
      region = lang;
    }
    const auto indexOf = [&](const std::string& id) -> std::optional<std::size_t> {
      const auto it = std::ranges::find(langs, id);
      return it == langs.end() ? std::nullopt : std::optional<std::size_t>(it - langs.begin());
    };

    Flex* card = addSettingsGroupCard(content, i18n::tr("settings.language.title"), scale);
    auto langRow = labelRow("", i18n::tr("settings.language.language"), scale);
    langRow->addChild(select(labels, indexOf(lang), scale,
                             [set = ctx.setLocale, all = locale.locale(), langs](std::size_t index) {
                               std::vector<std::string> next{"LANG=" + langs[index]};
                               for (const auto& a : all) {
                                 if (!a.starts_with("LANG=")) {
                                   next.push_back(a);
                                 }
                               }
                               set(std::move(next));
                             }));
    card->addChild(std::move(langRow));
    auto regionRow = labelRow("", i18n::tr("settings.language.region"), scale);
    regionRow->addChild(select(std::move(labels), indexOf(region), scale,
                               [set = ctx.setLocale, all = locale.locale(), langs](std::size_t index) {
                                 std::vector<std::string> next;
                                 for (const auto& a : all) {
                                   const auto eq = a.find('=');
                                   if (!std::ranges::contains(kRegionKeys, std::string_view(a).substr(0, eq))) {
                                     next.push_back(a);
                                   }
                                 }
                                 for (const auto key : kRegionKeys) {
                                   next.push_back(std::string(key) + "=" + langs[index]);
                                 }
                                 set(std::move(next));
                               }));
    card->addChild(std::move(regionRow));
    addSettingsRowSeparators(*card, scale);
    addFootnote(*card, locale.lastError().empty() ? formatPreview(region) : locale.lastError(), scale);

    Flex* keyboard = addSettingsGroupCard(content, i18n::tr("settings.language.keyboard"), scale);
    if (ctx.openKeyboard) {
      addInfoRow(*keyboard, "keyboard", i18n::tr("settings.language.input-sources"), {}, scale, ctx.openKeyboard);
    }
    std::vector<std::string> layoutLabels;
    std::vector<std::string> layoutNames;
    std::optional<std::size_t> layoutSelected;
    for (const xkb::Layout& layout : ctx.catalog->layouts) {
      if (layout.name == "custom") {
        continue;
      }
      if (layout.name == locale.x11Layout()) {
        layoutSelected = layoutNames.size();
      }
      layoutNames.push_back(layout.name);
      layoutLabels.push_back(layout.description);
    }
    auto layoutRow = labelRow("", i18n::tr("settings.language.keyboard-layout"), scale);
    layoutRow->addChild(select(std::move(layoutLabels), layoutSelected, scale,
                               [set = ctx.setX11Layout, layoutNames](std::size_t index) { set(layoutNames[index], ""); }));
    keyboard->addChild(std::move(layoutRow));
    addSettingsRowSeparators(*keyboard, scale);
    addFootnote(*keyboard, i18n::tr("settings.language.keyboard-footnote"), scale);
  }

} // namespace settings
