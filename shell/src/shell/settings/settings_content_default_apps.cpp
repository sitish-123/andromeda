#include "shell/settings/settings_content_default_apps.h"

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

    std::string_view categoryLabelKey(default_apps::Category category) {
      switch (category) {
      case default_apps::Category::WebBrowser:
        return "settings.default-apps.web-browser";
      case default_apps::Category::Mail:
        return "settings.default-apps.mail";
      case default_apps::Category::FileManager:
        return "settings.default-apps.file-manager";
      case default_apps::Category::Terminal:
        return "settings.default-apps.terminal";
      case default_apps::Category::TextEditor:
        return "settings.default-apps.text-editor";
      case default_apps::Category::ImageViewer:
        return "settings.default-apps.image-viewer";
      case default_apps::Category::VideoPlayer:
        return "settings.default-apps.video-player";
      case default_apps::Category::MusicPlayer:
        return "settings.default-apps.music-player";
      case default_apps::Category::PdfViewer:
        return "settings.default-apps.pdf-viewer";
      }
      return {};
    }

    void addCategoryRow(Flex& body, const SettingsDefaultAppsContext& ctx, default_apps::Category category) {
      const std::vector<const DesktopEntry*> candidates = default_apps::candidatesForCategory(ctx.entries, category);
      if (candidates.empty()) {
        return;
      }
      const std::string current = ctx.currentDefault(category);
      std::vector<std::string> labels;
      std::vector<std::string> ids;
      std::optional<std::size_t> selected;
      for (const DesktopEntry* entry : candidates) {
        const std::string id = entry->id + ".desktop";
        if (id == current) {
          selected = ids.size();
        }
        ids.push_back(id);
        labels.push_back(entry->name);
      }
      body.addChild(makeRow(
          i18n::tr(categoryLabelKey(category)),
          ui::select({
              .options = std::move(labels),
              .selectedIndex = selected,
              .fontSize = Style::fontSizeBody * ctx.scale,
              .controlHeight = Style::controlHeight * ctx.scale,
              .glyphSize = Style::fontSizeBody * ctx.scale,
              .width = kSelectWidth * ctx.scale,
              .height = Style::controlHeight * ctx.scale,
              .onSelectionChanged = [set = ctx.setDefault, category, ids = std::move(ids)](
                                        std::size_t index, std::string_view
                                    ) { set(category, ids[index]); },
          }),
          ctx.scale
      ));
    }

  } // namespace

  void addSettingsDefaultApps(Flex& content, const SettingsDefaultAppsContext& ctx) {
    Flex* body = addSettingsCard(content, i18n::tr("settings.default-apps.title"), ctx.scale);
    for (const default_apps::CategorySpec& spec : default_apps::categories()) {
      addCategoryRow(*body, ctx, spec.category);
    }
  }

} // namespace settings
