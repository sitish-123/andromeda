#include "shell/settings/settings_content_devices.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

namespace settings {

  namespace {

    std::unique_ptr<Flex> makeCaption(std::string_view text, float scale) {
      auto column = ui::column({.align = FlexAlign::Start, .fillWidth = true});
      column->addChild(makeLabel(text, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)));
      return column;
    }

    std::unique_ptr<Flex> makePhoneRow(const SettingsDevicePhone& phone, const SettingsDevicesContext& ctx) {
      const float scale = ctx.scale;
      auto status = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::box({
              .fill = colorSpecFromRole(phone.connected ? ColorRole::Primary : ColorRole::OutlineVariant),
              .radius = 4.0F * scale,
              .width = 8.0F * scale,
              .height = 8.0F * scale,
          }),
          makeLabel(
              i18n::tr(phone.connected ? "settings.devices.connected" : "settings.devices.not-connected"),
              Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
          )
      );
      return ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceMd * scale,
           .paddingV = Style::spaceSm * scale,
           .paddingH = Style::spaceMd * scale,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .radius = Style::scaledRadiusMd(scale),
           .minHeight = Style::controlHeightLg * scale,
           .fillWidth = true},
          ui::glyph({
              .glyph = "device-mobile",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(phone.connected ? ColorRole::Primary : ColorRole::OnSurface),
          }),
          ui::column(
              {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
              makeLabel(
                  phone.name, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
              ),
              std::move(status)
          ),
          ui::button({
              .text = i18n::tr("settings.devices.unpair"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .variant = ButtonVariant::Ghost,
              .onClick = [unpair = ctx.unpair, id = phone.id]() { unpair(id); },
          })
      );
    }

    void addPhones(Flex& content, const SettingsDevicesContext& ctx) {
      Flex* card = addSettingsCard(content, i18n::tr("settings.devices.phones"), ctx.scale);
      if (!ctx.linkAvailable) {
        card->addChild(makeCaption(i18n::tr("settings.devices.unavailable"), ctx.scale));
        return;
      }
      if (ctx.phones.empty()) {
        card->addChild(makeCaption(i18n::tr("settings.devices.none"), ctx.scale));
      }
      for (const auto& phone : ctx.phones) {
        card->addChild(makePhoneRow(phone, ctx));
      }
      auto pairRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * ctx.scale, .fillWidth = true});
      pairRow->addChild(
          ui::button({
              .text = i18n::tr("settings.devices.pair"),
              .glyph = "qrcode",
              .fontSize = Style::fontSizeBody * ctx.scale,
              .controlHeight = Style::controlHeight * ctx.scale,
              .variant = ButtonVariant::Default,
              .onClick = ctx.pair,
          })
      );
      pairRow->addChild(makeCaption(i18n::tr("settings.devices.pair-hint"), ctx.scale));
      card->addChild(std::move(pairRow));
    }

    void addCommands(Flex& content, const SettingsDevicesContext& ctx) {
      if (!ctx.linkAvailable) {
        return;
      }
      const float scale = ctx.scale;
      Flex* card = addSettingsCard(content, i18n::tr("settings.devices.commands"), scale);
      card->addChild(makeCaption(i18n::tr("settings.devices.commands-hint"), scale));
      for (std::size_t i = 0; i < ctx.commands.size(); ++i) {
        const auto& [name, command] = ctx.commands[i];
        card->addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true},
                ui::column(
                    {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
                    makeLabel(
                        name, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
                    ),
                    makeLabel(command, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
                ),
                ui::button({
                    .glyph = "trash",
                    .fontSize = Style::fontSizeBody * scale,
                    .controlHeight = Style::controlHeight * scale,
                    .variant = ButtonVariant::Ghost,
                    .tooltip = i18n::tr("settings.devices.command-remove"),
                    .onClick = [commands = ctx.commands, set = ctx.setCommands, i]() mutable {
                      commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(i));
                      set(std::move(commands));
                    },
                })
            )
        );
      }
      Input* name = nullptr;
      Input* command = nullptr;
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::input({
              .out = &name,
              .placeholder = i18n::tr("settings.devices.command-name"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .flexGrow = 1.0F,
          })
      );
      row->addChild(
          ui::input({
              .out = &command,
              .placeholder = i18n::tr("settings.devices.command-line"),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceSm * scale,
              .flexGrow = 2.0F,
          })
      );
      const auto add = [name, command, commands = ctx.commands, set = ctx.setCommands]() {
        const std::string label = StringUtils::trim(name->value());
        const std::string line = StringUtils::trim(command->value());
        name->setInvalid(label.empty());
        command->setInvalid(line.empty());
        if (!label.empty() && !line.empty()) {
          auto next = commands;
          next.emplace_back(label, line);
          set(std::move(next));
        }
      };
      row->addChild(
          ui::button({
              .text = i18n::tr("settings.devices.command-add"),
              .glyph = "plus",
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .variant = ButtonVariant::Default,
              .onClick = add,
          })
      );
      command->setOnSubmit([add](const std::string& /*text*/) { add(); });
      card->addChild(std::move(row));
    }

    void addQuickShare(Flex& content, const SettingsDevicesContext& ctx) {
      Flex* card = addSettingsCard(content, i18n::tr("settings.devices.quick-share"), ctx.scale);
      if (!ctx.quickShareAvailable) {
        card->addChild(makeCaption(i18n::tr("settings.devices.unavailable"), ctx.scale));
        return;
      }
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale, .fillWidth = true});
      row->addChild(makeLabel(
          i18n::tr("settings.devices.visible"), Style::fontSizeBody * ctx.scale,
          colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      ));
      row->addChild(ui::spacer());
      row->addChild(
          ui::toggle({
              .checked = ctx.quickShareVisible,
              .scale = ctx.scale,
              .onChange = ctx.setQuickShareVisible,
          })
      );
      card->addChild(std::move(row));
      card->addChild(makeCaption(i18n::tr("settings.devices.visible-hint", "name", ctx.quickShareName), ctx.scale));
      card->addChild(makeCaption(i18n::tr("settings.devices.downloads", "path", ctx.downloads), ctx.scale));
    }

  } // namespace

  void addSettingsDevices(Flex& content, const SettingsDevicesContext& ctx) {
    addPhones(content, ctx);
    addCommands(content, ctx);
    addQuickShare(content, ctx);
  }

} // namespace settings
