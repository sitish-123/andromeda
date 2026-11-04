#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_pages.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    // Shows at most this many results: a one-letter query matches hundreds of rows.
    constexpr std::size_t kMaxSearchResults = 50;

    // One search hit: a bar row is shown once for all bars that have it.
    struct Hit {
      const SettingEntry* entry;
      int rank;
      std::vector<std::string> bars;
    };

    // Lower is better: exact title, title prefix, title substring, config key, any other text.
    int matchRank(const SettingEntry& entry, std::string_view query) {
      const std::string title = StringUtils::toLower(entry.title);
      if (title == query) {
        return 0;
      }
      if (title.starts_with(query)) {
        return 1;
      }
      if (title.contains(query)) {
        return 2;
      }
      return StringUtils::toLower(settingEntryKey(entry)).contains(query) ? 3 : 4;
    }

    // Identity of a row across bars: the config path without the bar name.
    std::string barlessKey(const SettingEntry& entry) {
      auto path = entry.path;
      path[1].clear();
      std::string key = std::string(settingsSectionId(entry.section));
      for (const auto& part : path) {
        key += '.' + part;
      }
      return key + '/' + entry.title;
    }

    std::string resultCaption(const Hit& hit) {
      const auto& entry = *hit.entry;
      const auto& page = settingsPageForEntry(entry);
      std::string caption = i18n::tr(settingsPageLabelKey(page));
      if (!hit.bars.empty()) {
        caption += " / ";
        for (std::size_t i = 0; i < hit.bars.size(); ++i) {
          caption += (i == 0 ? "" : ", ") + hit.bars[i];
        }
      }
      if (!entry.group.empty()) {
        caption += " / " + i18n::tr("settings.navigation.groups." + entry.group);
      }
      return caption;
    }

    std::unique_ptr<Button> makeResultRow(const Hit& hit, const SettingsContentContext& ctx) {
      const auto& entry = *hit.entry;
      const float scale = ctx.scale;
      const auto& page = settingsPageForEntry(entry);
      auto row = ui::button({
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .minHeight = Style::controlHeightLg * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceXs * scale,
          .gap = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [open = ctx.openSearchResult, entry]() {
            if (open) {
              open(entry);
            }
          },
      });
      row->addChild(
          ui::row(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .fill = frameColors().tileFill,
               .radius = Style::scaledRadiusSm(scale),
               .minWidth = metrics::kResultTile * scale,
               .minHeight = metrics::kResultTile * scale,
               .width = metrics::kResultTile * scale,
               .height = metrics::kResultTile * scale},
              ui::glyph({
                  .glyph = std::string(page.glyph),
                  .glyphSize = Style::baseGlyphSize * scale,
                  .color = colorSpecFromRole(ColorRole::Label1),
              })
          )
      );
      row->addChild(
          ui::column(
              {.align = FlexAlign::Start, .justify = FlexJustify::Center, .flexGrow = 1.0F},
              makeLabel(entry.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1), FontWeight::Bold),
              makeLabel(resultCaption(hit), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2))
          )
      );
      row->addChild(
          ui::glyph({
              .glyph = "chevron-right",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Label3),
          })
      );
      return row;
    }

  } // namespace

  std::size_t
  addSettingsSearchResults(Flex& content, const std::vector<SettingEntry>& registry, const SettingsContentContext& ctx) {
    const float scale = ctx.scale;
    const std::string query = normalizedSettingQuery(ctx.searchQuery);
    std::vector<Hit> hits;
    std::unordered_map<std::string, std::size_t> barHitIndex;
    for (const auto& entry : registry) {
      if ((entry.visibleWhen && !entry.visibleWhen(ctx.config)) || !matchesNormalizedSettingQuery(entry, query)) {
        continue;
      }
      if (entry.section == SettingsSection::Bar && entry.path.size() >= 2 && entry.path[0] == "bar") {
        const auto [it, inserted] = barHitIndex.try_emplace(barlessKey(entry), hits.size());
        if (!inserted) {
          hits[it->second].bars.push_back(entry.path[1]);
          continue;
        }
        hits.push_back(Hit{&entry, matchRank(entry, query), {entry.path[1]}});
        continue;
      }
      hits.push_back(Hit{&entry, matchRank(entry, query), {}});
    }
    std::ranges::stable_sort(hits, {}, &Hit::rank);

    Flex* body = nullptr;
    std::size_t count = 0;
    bool truncated = false;
    for (const auto& hit : hits) {
      if (count >= kMaxSearchResults) {
        truncated = true;
        break;
      }
      if (body == nullptr) {
        body = addSettingsGroupCard(content, {}, scale);
      }
      body->addChild(makeResultRow(hit, ctx));
      ++count;
    }
    if (body != nullptr) {
      addSettingsRowSeparators(*body, scale);
    }
    if (count == 0) {
      content.addChild(
          ui::column(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .gap = Style::spaceSm * scale,
               .padding = (Style::spaceLg * 2.0F) * scale,
               .minHeight = metrics::kEmptyStateMinHeight * scale},
              makeLabel(
                  i18n::tr("settings.window.no-results"), Style::fontSizeHeader * scale,
                  colorSpecFromRole(ColorRole::Label1), FontWeight::Bold
              ),
              makeLabel(
                  i18n::tr("settings.window.no-results-hint"), Style::fontSizeBody * scale,
                  colorSpecFromRole(ColorRole::Label2)
              )
          )
      );
    }
    if (truncated) {
      content.addChild(makeLabel(
          i18n::tr("settings.window.search-truncated", "count", std::to_string(kMaxSearchResults)),
          Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label2)
      ));
    }
    return count;
  }

} // namespace settings
