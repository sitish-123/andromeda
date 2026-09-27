#include "shell/settings/settings_content_devices.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace settings {

  namespace {

    std::unique_ptr<Flex> makeCaption(std::string_view text, float scale) {
      auto column = ui::column({.align = FlexAlign::Start, .fillWidth = true});
      column->addChild(
          makeLabel(text, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      );
      return column;
    }

    std::unique_ptr<Flex> makePhoneRow(const SettingsDevicePhone& phone, const SettingsDevicesContext& ctx) {
      const float scale = ctx.scale;
      auto status = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::box({
              .fill = colorSpecFromRole(phone.connected ? ColorRole::Primary : ColorRole::Outline),
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
      pairRow->addChild(ui::button({
          .text = i18n::tr("settings.devices.pair"),
          .glyph = "qrcode",
          .fontSize = Style::fontSizeBody * ctx.scale,
          .controlHeight = Style::controlHeight * ctx.scale,
          .variant = ButtonVariant::Default,
          .onClick = ctx.pair,
      }));
      pairRow->addChild(makeCaption(i18n::tr("settings.devices.pair-hint"), ctx.scale));
      card->addChild(std::move(pairRow));
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
      row->addChild(ui::toggle({
          .checked = ctx.quickShareVisible,
          .scale = ctx.scale,
          .onChange = ctx.setQuickShareVisible,
      }));
      card->addChild(std::move(row));
      card->addChild(makeCaption(i18n::tr("settings.devices.visible-hint", "name", ctx.quickShareName), ctx.scale));
      card->addChild(makeCaption(i18n::tr("settings.devices.downloads", "path", ctx.downloads), ctx.scale));
    }

  } // namespace

  void addSettingsDevices(Flex& content, const SettingsDevicesContext& ctx) {
    addPhones(content, ctx);
    addQuickShare(content, ctx);
  }

} // namespace settings
