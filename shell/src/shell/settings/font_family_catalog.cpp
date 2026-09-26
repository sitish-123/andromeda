#include "shell/settings/font_family_catalog.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"

#include <algorithm>
#include <fontconfig/fontconfig.h>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    constexpr Logger kLog("fonts");

    std::vector<std::string> discoverFontFamiliesUncached() {
      std::unordered_set<std::string> seen;
      FcPattern* pattern = FcPatternCreate();
      FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, nullptr);
      FcFontSet* fonts = FcFontList(nullptr, pattern, objects);
      if (fonts != nullptr) {
        for (int i = 0; i < fonts->nfont; ++i) {
          FcChar8* family = nullptr;
          for (int n = 0; FcPatternGetString(fonts->fonts[i], FC_FAMILY, n, &family) == FcResultMatch; ++n) {
            std::string name = StringUtils::trim(reinterpret_cast<const char*>(family));
            if (!name.empty()) {
              seen.insert(std::move(name));
            }
          }
        }
        FcFontSetDestroy(fonts);
      }
      FcObjectSetDestroy(objects);
      FcPatternDestroy(pattern);

      std::vector<std::string> families(seen.begin(), seen.end());
      std::ranges::sort(families, [](const std::string& a, const std::string& b) {
        return StringUtils::toLower(a) < StringUtils::toLower(b);
      });
      kLog.info("font catalog: {} families", families.size());
      return families;
    }

  } // namespace

  const std::vector<std::string>& discoverFontFamilies() {
    static const std::vector<std::string> kFamilies = discoverFontFamiliesUncached();
    return kFamilies;
  }

  std::vector<WidgetSettingSelectOption> buildFontFamilySelectOptions() {
    const std::vector<std::string>& families = discoverFontFamilies();

    std::vector<WidgetSettingSelectOption> options;
    options.reserve(families.size() + 1);
    options.push_back(WidgetSettingSelectOption{"", i18n::tr("desktop-widgets.editor.settings.font-family-default")});
    for (const std::string& family : families) {
      options.push_back(WidgetSettingSelectOption{family, family});
    }
    return options;
  }

} // namespace settings
