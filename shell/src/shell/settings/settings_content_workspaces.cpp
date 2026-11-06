#include "shell/settings/settings_content_workspaces.h"

#include "compositors/settings_control.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string_view>
#include <vector>

namespace settings {
  namespace {
    using namespace page_rows;

    constexpr float kSelectWidth = 240.0F;
    constexpr std::string_view kDetail = "workspaces:"; // registry_compositor's Per-Workspace Arrangement button
    // Picker order: "" follows the page's Window Arrangement, then the four modes as the overview chip lists them.
    constexpr std::array<std::string_view, 5> kModes{"", "floating", "scrolling", "dwindle", "master"};

    std::string modeLabel(std::string_view mode) {
      return i18n::tr("settings.schema.andromeda.layout-mode.options." + std::string(mode));
    }

    // Workspace numbers the compositor reported, ascending.
    std::vector<int> slots(const SettingsControl& compositor) {
      constexpr std::string_view prefix = "workspace.";
      constexpr std::string_view suffix = ".layout.mode";
      std::vector<int> out;
      for (const auto& [key, setting] : compositor.settings()) {
        if (!key.starts_with(prefix) || !key.ends_with(suffix) || key.size() <= prefix.size() + suffix.size()) {
          continue;
        }
        const std::string_view digits(key.data() + prefix.size(), key.size() - prefix.size() - suffix.size());
        int index = 0;
        if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc{} && index > 0) {
          out.push_back(index);
        }
      }
      std::ranges::sort(out);
      return out;
    }
  } // namespace

  void addSettingsWorkspaces(Flex& content, const SettingsWorkspacesContext& ctx) {
    if (ctx.compositor == nullptr || !ctx.compositor->ready()) {
      return;
    }
    SettingsControl& compositor = *ctx.compositor;
    const float scale = ctx.scale;
    const std::vector<int> workspaces = slots(compositor);

    if (!ctx.detail.starts_with(kDetail)) {
      return;
    }

    const std::string fallback = compositor.value("layout.mode");
    std::vector<std::string> labels;
    for (const std::string_view mode : kModes) {
      labels.push_back(
          mode.empty() ? i18n::tr("settings.windows.same-as-default", "mode", modeLabel(fallback)) : modeLabel(mode)
      );
    }
    Flex* card = addCard(content, scale);
    for (const int index : workspaces) {
      const std::string key = std::format("workspace.{}.layout.mode", index);
      const std::string value = compositor.value(key);
      const auto found = std::ranges::find(kModes, std::string_view(value));
      const std::size_t current = found == kModes.end() ? 0 : static_cast<std::size_t>(found - kModes.begin());
      auto r = row(scale);
      r->addChild(page_rows::title(i18n::tr("settings.windows.workspace", "n", std::to_string(index)), scale));
      r->addChild(ui::select({
          .options = labels,
          .selectedIndex = current,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = kSelectWidth * scale,
          .onSelectionChanged =
              [&compositor, key, current](std::size_t picked, std::string_view /*label*/) {
                if (picked != current && picked < kModes.size()) {
                  compositor.set(key, std::string(kModes[picked]));
                }
              },
      }));
      card->addChild(std::move(r));
    }
  }

} // namespace settings
