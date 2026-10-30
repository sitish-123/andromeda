#include "shell/settings/settings_content_devices.h"

#include "dbus/link/link_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace settings {

  void addSettingsDevices(Flex& content, float scale) {
    content.addChild(
        ui::row(
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
                .color = colorSpecFromRole(ColorRole::Primary),
            }),
            ui::column(
                {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
                makeLabel(
                    i18n::tr("settings.devices.phones"), Style::fontSizeBody * scale,
                    colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
                ),
                makeLabel(
                    i18n::tr("settings.devices.phones-hint"), Style::fontSizeCaption * scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant)
                )
            ),
            ui::button({
                .text = i18n::tr("settings.devices.open"),
                .glyph = "external-link",
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .variant = ButtonVariant::Default,
                .onClick = []() { LinkService::openApp(); },
            })
        )
    );
  }

} // namespace settings
