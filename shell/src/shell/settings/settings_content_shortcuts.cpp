#include "shell/settings/settings_content_shortcuts.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace settings {

  namespace {

    constexpr float kChordWidth = 180.0F;
    constexpr float kActionSelectWidth = 280.0F;
    constexpr float kArgumentWidth = 220.0F;

    std::string_view actionName(std::string_view action) { return action.substr(0, action.find(':')); }

    // Cards in display order; an action lands in the first group whose prefix it starts with.
    struct Group {
      std::string_view id;
      std::array<std::string_view, 3> prefixes;
    };
    constexpr std::array<Group, 7> kGroups{{
        {"apps", {"spawn", "shell", ""}},
        {"windows", {"window-", "column-", ""}},
        {"workspaces", {"workspace-", "output-", ""}},
        {"overview", {"overview", "scratchpad", ""}},
        {"system", {"session-", "config-", "vt-"}},
        {"other", {"", "", ""}},
        {"disabled", {"", "", ""}},
    }};

    std::string_view groupFor(const CompositorKeybind& bind) {
      if (bind.action == "none") {
        return "disabled";
      }
      const std::string_view name = actionName(bind.action);
      for (const Group& group : kGroups) {
        if (std::ranges::any_of(group.prefixes, [&](std::string_view p) { return !p.empty() && name.starts_with(p); })) {
          return group.id;
        }
      }
      return "other";
    }

    const CompositorAction* findAction(const SettingsControl& compositor, std::string_view name) {
      const auto it = std::ranges::find(compositor.actions(), name, &CompositorAction::name);
      return it != compositor.actions().end() ? &*it : nullptr;
    }

    std::unique_ptr<Button> makeButton(
        std::string text, std::string glyph, ButtonVariant variant, float scale, std::function<void()> onClick,
        bool enabled = true, std::string tooltip = {}
    ) {
      ui::ButtonProps props;
      if (!text.empty()) {
        props.text = std::move(text);
      }
      if (!glyph.empty()) {
        props.glyph = std::move(glyph);
        props.glyphSize = Style::fontSizeBody * scale;
      }
      if (!tooltip.empty()) {
        props.tooltip = std::move(tooltip);
      }
      props.fontSize = Style::fontSizeCaption * scale;
      props.enabled = enabled;
      props.variant = variant;
      props.minHeight = Style::controlHeightSm * scale;
      props.paddingV = Style::spaceXs * scale;
      props.paddingH = Style::spaceSm * scale;
      props.radius = Style::scaledRadiusMd(scale);
      props.onClick = std::move(onClick);
      return ui::button(std::move(props));
    }

    // The chord control: shows the chord, and while recording asks for the keys. Clicking it again cancels.
    std::unique_ptr<Button> makeChordButton(
        const SettingsShortcutsContext& ctx, std::string row, std::string_view shown,
        std::function<void(std::string)> onCaptured
    ) {
      const bool recording = ctx.recordingRow == row;
      auto button = makeButton(
          recording ? i18n::tr("settings.shortcuts.press-keys")
                    : (shown.empty() ? i18n::tr("settings.shortcuts.set-keys") : std::string(shown)),
          "keyboard", recording ? ButtonVariant::Primary : ButtonVariant::Default, ctx.scale,
          [record = ctx.record, cancel = ctx.cancelRecording, recording, row, onCaptured = std::move(onCaptured)]() {
            if (recording) {
              cancel();
            } else {
              record(row, onCaptured);
            }
          }
      );
      button->setMinWidth(kChordWidth * ctx.scale);
      return button;
    }

    void addShortcutRow(Flex& body, const SettingsShortcutsContext& ctx, const CompositorKeybind& bind) {
      const float scale = ctx.scale;
      const bool disabled = bind.action == "none";
      const CompositorAction* action = findAction(*ctx.compositor, actionName(bind.action));
      const std::string title = disabled ? bind.chord
          : action != nullptr && !action->summary.empty() ? action->summary
                                                          : std::string(actionName(bind.action));

      auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
      copy->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      copy->addChild(makeSettingSubtitleLabel(disabled ? i18n::tr("settings.shortcuts.unbound") : bind.action, scale));

      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(std::move(copy));
      if (bind.customized) {
        row->addChild(makeButton(
            {}, "arrow-back-up", ButtonVariant::Ghost, scale,
            [bindFn = ctx.bind, chord = bind.chord]() { bindFn(chord, ""); }, true,
            i18n::tr("settings.actions.reset-to-default")
        ));
      }
      if (!disabled) {
        // Moving a shortcut binds the new chord and unbinds the old one, which may be a built-in underneath.
        row->addChild(makeChordButton(
            ctx, bind.chord, bind.chord,
            [bindFn = ctx.bind, oldChord = bind.chord, actionText = bind.action](std::string captured) {
              if (!captured.empty() && captured != oldChord) {
                bindFn(captured, actionText);
                bindFn(oldChord, "none");
              }
            }
        ));
        row->addChild(makeButton(
            {}, "trash", ButtonVariant::Ghost, scale, [bindFn = ctx.bind, chord = bind.chord]() { bindFn(chord, "none"); },
            true, i18n::tr("settings.shortcuts.remove")
        ));
      }
      body.addChild(std::move(row));
    }

    void addDraftCard(Flex& content, const SettingsShortcutsContext& ctx) {
      const float scale = ctx.scale;
      Flex* body = addSettingsCard(content, i18n::tr("settings.shortcuts.add-title"), scale);
      ShortcutDraft* draft = &ctx.draft;

      std::vector<std::string> labels;
      std::vector<std::string> names;
      std::optional<std::size_t> selected;
      for (const CompositorAction& action : ctx.compositor->actions()) {
        if (action.name == draft->action) {
          selected = names.size();
        }
        names.push_back(action.name);
        labels.push_back(action.summary.empty() ? action.name : action.name + " — " + action.summary);
      }
      const CompositorAction* chosen = findAction(*ctx.compositor, draft->action);

      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(ui::select({
          .options = std::move(labels),
          .selectedIndex = selected,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = kActionSelectWidth * scale,
          .height = Style::controlHeight * scale,
          .onSelectionChanged = [draft, names = std::move(names), rebuild = ctx.requestRebuild](
                                    std::size_t index, std::string_view
                                ) {
            draft->action = names[index];
            draft->argument.clear();
            rebuild();
          },
      }));
      if (chosen != nullptr && !chosen->param.empty()) {
        row->addChild(ui::input({
            .value = draft->argument,
            .placeholder = chosen->param,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = Style::spaceSm * scale,
            .width = kArgumentWidth * scale,
            .height = Style::controlHeight * scale,
            .onChange = [draft](const std::string& value) { draft->argument = value; },
        }));
      }
      row->addChild(ui::spacer());
      row->addChild(makeChordButton(
          ctx, std::string(kDraftChordRow), draft->chord, [draft](std::string captured) {
            if (!captured.empty()) {
              draft->chord = std::move(captured);
            }
          }
      ));
      const bool complete = chosen != nullptr && !draft->chord.empty() && (chosen->param.empty() || !draft->argument.empty());
      row->addChild(makeButton(
          i18n::tr("settings.shortcuts.add"), "add", ButtonVariant::Primary, scale,
          [draft, bindFn = ctx.bind, needsArgument = chosen != nullptr && !chosen->param.empty()]() {
            bindFn(draft->chord, needsArgument ? draft->action + ":" + draft->argument : draft->action);
            *draft = {};
          },
          complete
      ));
      body->addChild(std::move(row));
    }

  } // namespace

  void addSettingsShortcuts(Flex& content, const SettingsShortcutsContext& ctx) {
    if (ctx.compositor == nullptr || !ctx.compositor->ready()) {
      content.addChild(makeSettingSubtitleLabel(i18n::tr("settings.shortcuts.unavailable"), ctx.scale));
      return;
    }
    addDraftCard(content, ctx);
    for (const Group& group : kGroups) {
      Flex* body = nullptr;
      for (const CompositorKeybind& bind : ctx.compositor->keybinds()) {
        if (groupFor(bind) != group.id) {
          continue;
        }
        if (body == nullptr) {
          body = addSettingsCard(content, i18n::tr("settings.shortcuts.groups." + std::string(group.id)), ctx.scale);
        }
        addShortcutRow(*body, ctx, bind);
      }
    }
  }

} // namespace settings
