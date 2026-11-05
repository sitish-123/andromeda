#include "shell/settings/settings_content_shortcuts.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "shell/settings/settings_selection_style.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace settings {

  namespace {

    constexpr float kRailWidth = 168.0F;
    constexpr std::size_t kMaxPickerRows = 8;
    // The segmented Search | Press control; the search field takes the rest of the pane row.
    constexpr float kModeSwitchWidth = 184.0F;

    // The rows pane: the page column less the rail, the gap and the scroll bar.
    float paneWidth(float scale) {
      return (metrics::kContentMaxWidth - kRailWidth - Style::spaceLg - Style::spaceLg) * scale;
    }

    constexpr std::array<std::string_view, 7> kCategories{
        "apps", "shell", "windows", "workspaces", "screenshots", "media", "custom",
    };

    std::string_view actionName(std::string_view action) { return action.substr(0, action.find(':')); }

    std::string_view actionArgument(std::string_view action) {
      const std::size_t colon = action.find(':');
      return colon == std::string_view::npos ? std::string_view{} : action.substr(colon + 1);
    }

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::string firstWord(std::string_view text) {
      const std::size_t start = text.find_first_not_of(' ');
      if (start == std::string_view::npos) {
        return {};
      }
      const std::size_t end = text.find(' ', start);
      return std::string(text.substr(start, end == std::string_view::npos ? end : end - start));
    }

    std::string basename(std::string_view path) {
      const std::size_t slash = path.rfind('/');
      return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }

    // "screenshot-region" reads as "Screenshot region".
    std::string humanize(std::string_view verb) {
      std::string text(verb);
      std::ranges::replace(text, '-', ' ');
      if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
      }
      return text;
    }

    // A desktop-shell command: the argument of shell:, or a spawn of "noctalia msg ...".
    std::optional<std::string> shellCommand(std::string_view action) {
      const std::string_view name = actionName(action);
      const std::string_view argument = actionArgument(action);
      if (name == "shell") {
        return std::string(argument);
      }
      constexpr std::string_view kMsg = "noctalia msg ";
      if (name == "spawn" && argument.starts_with(kMsg)) {
        return std::string(argument.substr(kMsg.size()));
      }
      return std::nullopt;
    }

    std::string shellTitle(std::string_view command) {
      const std::string verb = firstWord(command);
      std::string_view rest = command.substr(std::min(command.size(), verb.size()));
      rest.remove_prefix(std::min(rest.size(), rest.find_first_not_of(' ')));
      if (verb == "panel-toggle" || verb == "panel-open") {
        static const std::map<std::string, std::string, std::less<>> kPanels{
            {"launcher", "Launcher"},         {"clipboard", "Clipboard"},
            {"control-center", "Control Center"}, {"notifications", "Notifications"},
            {"session", "Power Menu"},        {"wallpaper", "Wallpaper"},
            {"display-mode", "Display Mode"},
        };
        const std::string panel = firstWord(rest);
        if (panel == "launcher" && rest.find("/emo") != std::string_view::npos) {
          return "Emoji";
        }
        const auto it = kPanels.find(panel);
        return it != kPanels.end() ? it->second : humanize(panel);
      }
      if (verb == "session") {
        return rest == "lock" ? "Lock Screen" : humanize(rest);
      }
      if (verb == "settings-toggle" || verb == "settings-open") {
        return "Settings";
      }
      if (verb == "activity-open") {
        return "Activity";
      }
      std::string title = humanize(verb);
      if (!rest.empty() && verb == "media") {
        title += " " + std::string(rest);
      }
      return title;
    }

    std::string_view shellCategory(std::string_view command, std::string_view chord) {
      if (command.starts_with("screenshot") || command.starts_with("screen-record") || command.starts_with("annotate")) {
        return "screenshots";
      }
      static constexpr std::array<std::string_view, 6> kMedia{
          "volume-", "brightness-", "media", "mic-", "keyboard-backlight", "osd",
      };
      if (chord.starts_with("XF86") || std::ranges::any_of(kMedia, [&](std::string_view p) {
            return command.starts_with(p);
          })) {
        return "media";
      }
      return "shell";
    }

    std::string execProgram(std::string_view exec) {
      std::string program = firstWord(exec);
      if (program == "env" || program == "flatpak") {
        return std::string(exec);
      }
      return basename(program);
    }

    // The command without desktop-entry field codes (%U, %f, ...).
    std::string execCommand(std::string_view exec) {
      std::string out;
      std::size_t pos = 0;
      while (pos < exec.size()) {
        const std::size_t end = std::min(exec.find(' ', pos), exec.size());
        const std::string_view word = exec.substr(pos, end - pos);
        if (!word.empty() && !(word.size() == 2 && word[0] == '%')) {
          if (!out.empty()) {
            out += ' ';
          }
          out += word;
        }
        pos = end + 1;
      }
      return out;
    }

    const DesktopEntry* entryForCommand(std::span<const DesktopEntry> entries, std::string_view command) {
      const std::string program = basename(firstWord(command));
      // A shell script is not the app whose launcher also starts with a shell.
      if (program.empty() || program == "sh" || program == "bash") {
        return nullptr;
      }
      for (const DesktopEntry& entry : entries) {
        if (entry.hidden || entry.noDisplay) {
          continue;
        }
        if (execProgram(entry.exec) == program || entry.id == program) {
          return &entry;
        }
      }
      return nullptr;
    }

    const CompositorAction* findAction(const SettingsControl& compositor, std::string_view name) {
      const auto it = std::ranges::find(compositor.actions(), name, &CompositorAction::name);
      return it != compositor.actions().end() ? &*it : nullptr;
    }

    std::string_view actionCategory(std::string_view name) {
      if (name.starts_with("workspace-") || name.starts_with("output-") || name.starts_with("overview")
          || name.starts_with("scratchpad") || name.starts_with("window-move-to-workspace")) {
        return "workspaces";
      }
      if (name.starts_with("window-") || name.starts_with("column-") || name.starts_with("layout-")) {
        return "windows";
      }
      return "shell";
    }

    // One row of the page: an action and every chord bound to it.
    struct Row {
      std::string category;
      std::string title;
      std::string detail; // Full command, shown as the tooltip.
      std::string action;
      const DesktopEntry* entry = nullptr;
      std::vector<std::string> chords;
      bool customized = false;
    };

    Row describe(const SettingsShortcutsContext& ctx, const CompositorKeybind& bind) {
      Row row{.action = bind.action};
      if (const auto command = shellCommand(bind.action); command.has_value()) {
        row.category = std::string(shellCategory(*command, bind.chord));
        row.title = shellTitle(*command);
        return row;
      }
      const std::string_view name = actionName(bind.action);
      const std::string_view argument = actionArgument(bind.action);
      if (name == "spawn") {
        row.detail = std::string(argument);
        if (const DesktopEntry* entry = entryForCommand(ctx.entries, argument); entry != nullptr) {
          row.category = "apps";
          row.title = entry->name;
          row.entry = entry;
        } else {
          row.category = "custom";
          row.title = basename(firstWord(argument));
          // The default terminal bind runs the first installed terminal from a list.
          if (argument.find("command -v $t") != std::string_view::npos) {
            row.category = "apps";
            row.title = "Terminal";
          } else if (row.title == "sh" || row.title == "bash") {
            row.title = std::string(argument);
          }
        }
        return row;
      }
      const CompositorAction* action = findAction(*ctx.compositor, name);
      row.category = std::string(actionCategory(name));
      row.title = action != nullptr && !action->summary.empty() ? action->summary : humanize(name);
      if (!argument.empty()) {
        row.title += " " + std::string(argument);
      }
      return row;
    }

    std::vector<Row> collectRows(const SettingsShortcutsContext& ctx) {
      std::vector<Row> rows;
      for (const CompositorKeybind& bind : ctx.compositor->keybinds()) {
        if (bind.action == "none") {
          continue;
        }
        Row row = describe(ctx, bind);
        // The same action under several chords is one row with several key groups.
        auto same = std::ranges::find_if(rows, [&](const Row& r) {
          return r.category == row.category && r.title == row.title;
        });
        if (same == rows.end()) {
          row.chords.push_back(bind.chord);
          row.customized = bind.customized;
          rows.push_back(std::move(row));
        } else {
          same->chords.push_back(bind.chord);
          same->customized = same->customized || bind.customized;
        }
      }
      return rows;
    }

    std::unique_ptr<Button> makeButton(
        std::string text, ButtonVariant variant, float scale, std::function<void()> onClick, bool enabled = true
    ) {
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .enabled = enabled,
          .variant = variant,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    // The chord control: one keycap per key; while recording it asks for the keys and is outlined in accent.
    // Clicking it again cancels.
    std::unique_ptr<Button> makeChordButton(
        const SettingsShortcutsContext& ctx, std::string row, std::string_view shown,
        std::function<void(std::string)> onCaptured
    ) {
      const bool recording = ctx.recordingRow == row;
      const float scale = ctx.scale;
      const ColorSpec none = clearColorSpec();
      const Button::ButtonStateColors state{
          .bg = none, .border = recording ? colorSpecFromRole(ColorRole::Accent) : none,
          .label = colorSpecFromRole(ColorRole::Label2)
      };
      auto button = ui::button({
          .variant = ButtonVariant::Ghost,
          .customPalette = Button::ButtonPalette{
              .borderWidth = Style::focusRingWidth * scale, .normal = state, .hover = state, .pressed = state,
              .disabled = state
          },
          .minHeight = (metrics::kKeycapHeight + 2.0F * Style::spaceSm) * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::spaceSm * scale,
          .onClick = [record = ctx.record, cancel = ctx.cancelRecording, recording, row,
                      onCaptured = std::move(onCaptured)]() {
            if (recording) {
              cancel();
            } else {
              record(row, onCaptured);
            }
          },
          .configure = [](Button& chord) { chord.setTabStop(false); },
      });
      if (recording || shown.empty()) {
        button->addChild(makeLabel(
            i18n::tr(recording ? "settings.shortcuts.press-keys" : "settings.shortcuts.set-keys"),
            Style::fontSizeCaption * scale,
            colorSpecFromRole(recording ? ColorRole::AccentText : ColorRole::Label2), FontWeight::Medium
        ));
        return button;
      }
      for (std::size_t start = 0; start <= shown.size();) {
        const std::size_t plus = shown.find('+', start);
        const std::string_view part = shown.substr(start, plus == std::string_view::npos ? plus : plus - start);
        if (!part.empty()) {
          button->addChild(ui::row(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .paddingH = Style::spaceXs * scale,
               .radius = Style::spaceXs * scale,
               .border = colorSpecFromRole(ColorRole::Separator),
               .minWidth = metrics::kKeycapHeight * scale,
               .minHeight = metrics::kKeycapHeight * scale},
              makeLabel(std::string(part), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2), FontWeight::Medium)
          ));
        }
        if (plus == std::string_view::npos) {
          break;
        }
        start = plus + 1;
      }
      return button;
    }

    void addRow(Flex& body, const SettingsShortcutsContext& ctx, const Row& row) {
      const float scale = ctx.scale;
      auto line = page_rows::row(scale);
      if (row.entry != nullptr && ctx.appIcon) {
        if (auto icon = ctx.appIcon(*row.entry, Tokens::sizeSymbolL * scale); icon != nullptr) {
          line->addChild(std::move(icon));
        }
      }
      auto title = page_rows::title(row.title, scale);
      line->addChild(std::move(title));
      if (row.customized) {
        line->addChild(page_rows::iconButton(
            "arrow-back-up", i18n::tr("settings.actions.reset-to-default"), scale,
            [bindFn = ctx.bind, chords = row.chords]() {
              for (const std::string& chord : chords) {
                bindFn(chord, "");
              }
            }
        ));
      }
      for (const std::string& chord : row.chords) {
        // Moving a shortcut binds the new chord and unbinds the old one, which may be a built-in underneath.
        line->addChild(makeChordButton(
            ctx, chord, chord, [bindFn = ctx.bind, oldChord = chord, actionText = row.action](std::string captured) {
              if (!captured.empty() && captured != oldChord) {
                bindFn(captured, actionText);
                bindFn(oldChord, "none");
              }
            }
        ));
      }
      line->addChild(page_rows::iconButton(
          "trash", i18n::tr("settings.shortcuts.remove"), scale, [bindFn = ctx.bind, chords = row.chords]() {
            for (const std::string& chord : chords) {
              bindFn(chord, "none");
            }
          }
      ));
      body.addChild(std::move(line));
    }

    struct Choice {
      std::string title;
      std::string action;
      const DesktopEntry* entry = nullptr;
    };

    // What "+ Add…" offers in a category that is neither Apps nor a custom command.
    std::vector<Choice> actionChoices(const SettingsShortcutsContext& ctx, std::string_view category) {
      static constexpr std::array<std::string_view, 25> kShell{
          "panel-toggle launcher",       "panel-toggle launcher /emo",  "panel-toggle clipboard",
          "panel-toggle control-center", "panel-toggle notifications",  "panel-toggle session",
          "panel-toggle wallpaper",      "settings-toggle",             "activity-open",
          "window-switcher",             "session lock",                "screenshot-region",
          "screenshot-fullscreen",       "screenshot-window",           "screenshot-annotate",
          "screen-record-toggle",        "volume-up",                   "volume-down",
          "volume-mute",                 "mic-mute",                    "media toggle",
          "media next",                  "media previous",              "brightness-up",
          "brightness-down",
      };
      std::vector<Choice> out;
      for (std::string_view command : kShell) {
        if (shellCategory(command, {}) == category) {
          out.push_back({.title = shellTitle(command), .action = "shell:" + std::string(command)});
        }
      }
      if (category == "windows" || category == "workspaces" || category == "shell") {
        for (const CompositorAction& action : ctx.compositor->actions()) {
          if (!action.param.empty() || action.name == "shell" || action.name == "spawn"
              || actionCategory(action.name) != category) {
            continue;
          }
          out.push_back({.title = action.summary.empty() ? humanize(action.name) : action.summary, .action = action.name});
        }
      }
      return out;
    }

    std::vector<Choice> appChoices(const SettingsShortcutsContext& ctx) {
      std::vector<Choice> out;
      for (const DesktopEntry& entry : ctx.entries) {
        if (entry.hidden || entry.noDisplay || entry.exec.empty()) {
          continue;
        }
        out.push_back({.title = entry.name, .action = "spawn:" + execCommand(entry.exec), .entry = &entry});
      }
      std::ranges::sort(out, {}, [](const Choice& c) { return lower(c.title); });
      return out;
    }

    std::unique_ptr<Node> searchField(
        const SettingsShortcutsContext& ctx, float width, const std::string& value, std::string placeholder,
        std::function<void(const std::string&)> onChange
    ) {
      const float scale = ctx.scale;
      return ui::input({
          .value = value,
          .placeholder = std::move(placeholder),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .width = width,
          .height = Style::controlHeight * scale,
          .onChange = std::move(onChange),
      });
    }

    void addAddForm(Flex& pane, const SettingsShortcutsContext& ctx, const std::vector<Row>& rows) {
      const float scale = ctx.scale;
      ShortcutDraft* draft = &ctx.draft;
      Flex* card = page_rows::addCard(pane, scale);
      const float formWidth = paneWidth(scale) - 2.0F * Style::spaceMd * scale;
      auto form = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .padding = Style::spaceMd * scale, .fillWidth = true});

      const bool custom = draft->category == "custom";
      if (custom) {
        form->addChild(ui::segmented({
            .options = std::vector<ui::SegmentedOption>{
                ui::SegmentedOption{.label = i18n::tr("settings.shortcuts.kind-app")},
                ui::SegmentedOption{.label = i18n::tr("settings.shortcuts.kind-command")}},
            .selectedIndex = draft->addCommand ? std::size_t{1} : std::size_t{0},
            .scale = scale,
            .onChange = [draft, rebuild = ctx.requestRebuild](std::size_t index) {
              draft->addCommand = index == 1;
              draft->addAction.clear();
              draft->addTitle.clear();
              rebuild();
            },
        }));
      }

      if (custom && draft->addCommand) {
        form->addChild(searchField(
            ctx, formWidth, draft->addCommandText, i18n::tr("settings.shortcuts.command-placeholder"),
            [draft](const std::string& text) {
              draft->addCommandText = text;
              draft->addAction = text.empty() ? std::string() : "spawn:" + text;
              draft->addTitle = basename(firstWord(text));
            }
        ));
      } else {
        const bool apps = draft->category == "apps" || custom;
        form->addChild(searchField(
            ctx, formWidth, draft->addQuery, i18n::tr(apps ? "settings.shortcuts.app-search" : "settings.shortcuts.pick-action"),
            [draft, rebuild = ctx.requestRebuild](const std::string& text) {
              draft->addQuery = text;
              rebuild();
            }
        ));
        const std::vector<Choice> choices = apps ? appChoices(ctx) : actionChoices(ctx, draft->category);
        const std::string needle = lower(draft->addQuery);
        auto list = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .fillWidth = true});
        std::size_t shown = 0;
        for (const Choice& choice : choices) {
          if (shown == kMaxPickerRows) {
            break;
          }
          if (!needle.empty() && lower(choice.title).find(needle) == std::string::npos) {
            continue;
          }
          ++shown;
          const bool selected = draft->addAction == choice.action;
          auto pick = ui::button({
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .customPalette = selected ? std::optional{selectedRowPalette(selectionStyle().rowFill, selectionStyle().rowLabel)}
                                        : std::nullopt,
              .minHeight = metrics::kSidebarRowHeight * scale,
              .paddingH = metrics::kSidebarRowPaddingH * scale,
              .gap = metrics::kSidebarRowGap * scale,
              .radius = Style::scaledRadiusSm(scale),
              .width = formWidth,
              .onClick = [draft, choice, rebuild = ctx.requestRebuild]() {
                draft->addAction = choice.action;
                draft->addTitle = choice.title;
                rebuild();
              },
          });
          if (choice.entry != nullptr && ctx.appIcon) {
            if (auto icon = ctx.appIcon(*choice.entry, Tokens::sizeSymbolL * scale); icon != nullptr) {
              pick->addChild(std::move(icon));
            }
          }
          pick->addChild(makeLabel(choice.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1)));
          list->addChild(std::move(pick));
        }
        form->addChild(std::move(list));
      }

      // The chord, a conflict line when another shortcut owns it, then Cancel and Add.
      auto footer = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      footer->addChild(makeChordButton(ctx, std::string(kDraftChordRow), draft->chord, [draft](std::string captured) {
        if (!captured.empty()) {
          draft->chord = std::move(captured);
        }
      }));
      footer->addChild(ui::spacer());
      footer->addChild(makeButton(i18n::tr("settings.shortcuts.cancel"), ButtonVariant::Ghost, scale, [draft, rebuild = ctx.requestRebuild]() {
        *draft = ShortcutDraft{.category = draft->category, .pressMode = draft->pressMode, .query = draft->query};
        rebuild();
      }));
      const bool complete = !draft->addAction.empty() && !draft->chord.empty();
      footer->addChild(makeButton(
          i18n::tr("settings.shortcuts.add"), ButtonVariant::Primary, scale,
          [draft, bindFn = ctx.bind, rebuild = ctx.requestRebuild]() {
            bindFn(draft->chord, draft->addAction);
            *draft = ShortcutDraft{.category = draft->category, .pressMode = draft->pressMode, .query = draft->query};
            rebuild();
          },
          complete
      ));
      if (!draft->chord.empty()) {
        const auto owner = std::ranges::find_if(rows, [&](const Row& r) {
          return std::ranges::contains(r.chords, draft->chord);
        });
        if (owner != rows.end()) {
          form->addChild(makeLabel(
              i18n::tr("settings.shortcuts.replaces", "name", owner->title), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::Warning)
          ));
        }
      }
      form->addChild(std::move(footer));
      card->addChild(std::move(form));
    }

    std::unique_ptr<Flex> makeRail(const SettingsShortcutsContext& ctx, const std::vector<Row>& rows) {
      const float scale = ctx.scale;
      ShortcutDraft* draft = &ctx.draft;
      auto rail = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .width = kRailWidth * scale});
      for (std::string_view category : kCategories) {
        const auto count = std::ranges::count(rows, category, &Row::category);
        const bool selected = !draft->pressMode && draft->query.empty() && draft->category == category;
        auto item = ui::button({
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .customPalette = selected ? std::optional{selectedRowPalette(selectionStyle().rowFill, selectionStyle().rowLabel)}
                                      : std::nullopt,
            .minHeight = metrics::kSidebarRowHeight * scale,
            .paddingH = metrics::kSidebarRowPaddingH * scale,
            .gap = metrics::kSidebarRowGap * scale,
            .radius = Style::scaledRadiusSm(scale),
            .width = kRailWidth * scale,
            .onClick = [draft, id = std::string(category), cancel = ctx.cancelRecording, rebuild = ctx.requestRebuild]() {
              cancel();
              *draft = ShortcutDraft{.category = id};
              rebuild();
            },
        });
        item->addChild(ui::row(
            {.align = FlexAlign::Center, .width = (kRailWidth - metrics::kSidebarRowPaddingH - metrics::kSidebarRowPaddingH) * scale},
            makeLabel(i18n::tr("settings.shortcuts.groups." + std::string(category)), Style::fontSizeBody * scale,
                      colorSpecFromRole(ColorRole::Label1)),
            ui::spacer(),
            makeLabel(std::to_string(count), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label3))
        ));
        rail->addChild(std::move(item));
      }
      return rail;
    }

  } // namespace

  void addSettingsShortcuts(Flex& content, const SettingsShortcutsContext& ctx) {
    if (ctx.compositor == nullptr || !ctx.compositor->ready()) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.shortcuts.unavailable"), ctx.scale));
      return;
    }
    const float scale = ctx.scale;
    ShortcutDraft* draft = &ctx.draft;
    const std::vector<Row> rows = collectRows(ctx);

    auto panes = ui::row({.align = FlexAlign::Start, .gap = Style::spaceLg * scale, .fillWidth = true});
    panes->addChild(makeRail(ctx, rows));

    auto pane = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .width = paneWidth(scale)});
    auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    top->addChild(ui::segmented({
        .options = std::vector<ui::SegmentedOption>{
            ui::SegmentedOption{.label = i18n::tr("settings.shortcuts.search")},
            ui::SegmentedOption{.label = i18n::tr("settings.shortcuts.press")}},
        .selectedIndex = draft->pressMode ? std::size_t{1} : std::size_t{0},
        .scale = scale,
        .onChange = [draft, cancel = ctx.cancelRecording, rebuild = ctx.requestRebuild](std::size_t index) {
          cancel();
          draft->pressMode = index == 1;
          draft->pressChord.clear();
          rebuild();
        },
    }));
    if (draft->pressMode) {
      top->addChild(makeChordButton(ctx, std::string(kPressChordRow), draft->pressChord, [draft](std::string captured) {
        draft->pressChord = std::move(captured);
      }));
    } else {
      top->addChild(searchField(
          ctx, paneWidth(scale) - (kModeSwitchWidth + Style::spaceSm) * scale, draft->query, i18n::tr("settings.shortcuts.search-placeholder"),
          [draft, rebuild = ctx.requestRebuild](const std::string& text) {
            draft->query = text;
            rebuild();
          }
      ));
    }
    pane->addChild(std::move(top));

    // Search and press look across every category; otherwise the rail's category shows.
    const std::string needle = lower(draft->query);
    std::vector<const Row*> shown;
    for (const Row& row : rows) {
      if (draft->pressMode) {
        if (!draft->pressChord.empty() && std::ranges::contains(row.chords, draft->pressChord)) {
          shown.push_back(&row);
        }
      } else if (!needle.empty()) {
        if (lower(row.title).find(needle) != std::string::npos || lower(row.detail).find(needle) != std::string::npos) {
          shown.push_back(&row);
        }
      } else if (row.category == draft->category) {
        shown.push_back(&row);
      }
    }

    const bool browsing = !draft->pressMode && needle.empty();
    if (shown.empty() && !browsing) {
      std::string text = draft->pressMode
          ? (draft->pressChord.empty() ? i18n::tr("settings.shortcuts.press-hint")
                                       : i18n::tr("settings.shortcuts.no-press-match", "keys", draft->pressChord))
          : i18n::tr("settings.shortcuts.no-results");
      pane->addChild(makeLabel(text, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label3)));
    }
    if (!shown.empty() || browsing) {
      Flex* card = page_rows::addCard(*pane, scale);
      for (const Row* row : shown) {
        addRow(*card, ctx, *row);
      }
      if (browsing && !draft->adding) {
        auto add = page_rows::row(scale);
        add->addChild(page_rows::plainButton(
            i18n::tr("settings.shortcuts.add-row"), ButtonVariant::Ghost, scale,
            [draft, rebuild = ctx.requestRebuild]() {
              draft->adding = true;
              rebuild();
            }
        ));
        card->addChild(std::move(add));
      }
    }
    if (browsing && draft->adding) {
      addAddForm(*pane, ctx, rows);
    }
    panes->addChild(std::move(pane));
    content.addChild(std::move(panes));
  }

} // namespace settings
