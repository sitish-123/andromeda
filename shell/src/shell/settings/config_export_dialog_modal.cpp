#include "shell/settings/config_export_dialog_modal.h"

#include "core/deferred_call.h"
#include "design_tokens.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

namespace settings {

  ConfigExportDialogModal::~ConfigExportDialogModal() {
    if (m_open) {
      close();
    }
    m_aliveGuard.reset();
  }

  void ConfigExportDialogModal::initialize(SettingsModalHost& host, std::function<void()> dismissSelectDropdown) {
    m_host = &host;
    m_dismissSelectDropdown = std::move(dismissSelectDropdown);
  }

  void ConfigExportDialogModal::open(ConfigExportDialogRequest request) {
    if (m_host == nullptr) {
      return;
    }
    if (m_open) {
      close();
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }

    m_scale = std::max(0.1F, request.scale);
    m_mode = ConfigExportMode::MergedUser;
    m_callback = std::move(request.callback);
    clearNodePointers();

    const float padding = 0.0F;
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_modalId = m_host->push(
        SettingsModalRequest{
            .build = [this, aliveGuard] -> std::unique_ptr<Node> { return aliveGuard.expired() ? nullptr : build(); },
            .measure =
                [this](Renderer& renderer, const SettingsModalLayoutSpace& space) { return measure(renderer, space); },
            .arrange = [this](Renderer& renderer, float width, float height) { arrange(renderer, width, height); },
            .initialFocusArea = [this] { return m_exportButton != nullptr ? m_exportButton->inputArea() : nullptr; },
            .requestClose =
                [this, aliveGuard] {
                  if (!aliveGuard.expired()) {
                    close();
                  }
                },
            .onClosed =
                [this, aliveGuard] {
                  if (!aliveGuard.expired()) {
                    m_open = false;
                    m_modalId.reset();
                    m_callback = nullptr;
                    clearNodePointers();
                  }
                },
            .contentPadding = padding,
            .windowMargin = 24.0F * m_scale,
            .scale = m_scale,
        }
    );
    m_open = m_modalId.has_value();
    if (!m_open) {
      m_callback = nullptr;
      clearNodePointers();
    }
  }

  void ConfigExportDialogModal::close() {
    if (!m_open || m_host == nullptr || !m_modalId.has_value()) {
      return;
    }
    if (m_dismissSelectDropdown) {
      m_dismissSelectDropdown();
    }
    (void)m_host->pop(*m_modalId);
  }

  std::unique_ptr<Node> ConfigExportDialogModal::build() {
    clearNodePointers();
    const float inset = Tokens::componentDialogPadding * m_scale;
    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
    });

    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * m_scale,
        .paddingH = inset,
        .minHeight = Tokens::componentDialogHeader * m_scale,
    });
    header->addChild(
        ui::label({
            .text = i18n::tr("settings.export-config.title"),
            .fontSize = Tokens::typeTitle3.size * m_scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::Label1),
            .flexGrow = 1.0F,
        })
    );
    if (Style::closeButtonsEnabled()) {
      header->addChild(
          ui::button({
              .glyph = "close",
              .glyphSize = Style::fontSizeBody * m_scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeight * m_scale,
              .minHeight = Style::controlHeight * m_scale,
              .padding = Style::spaceXs * m_scale,
              .radius = Style::scaledRadiusMd(m_scale),
              .onClick = [this] {
                const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                DeferredCall::callLater([this, aliveGuard] {
                  if (!aliveGuard.expired()) {
                    close();
                  }
                });
              },
          })
      );
    }
    root->addChild(std::move(header));

    auto body = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * m_scale, .paddingH = inset});
    body->addChild(
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceSm * m_scale,
            },
            makeOption(
                ConfigExportMode::MergedUser, i18n::tr("settings.export-config.merged-user-title"),
                i18n::tr("settings.export-config.merged-user-description")
            ),
            makeOption(
                ConfigExportMode::FullEffective, i18n::tr("settings.export-config.full-effective-title"),
                i18n::tr("settings.export-config.full-effective-description")
            )
        )
    );

    body->addChild(
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceXs * m_scale,
                .padding = Style::spaceSm * m_scale,
                .configure =
                    [this](Flex& warning) {
                      warning.setRadius(Style::scaledRadiusMd(m_scale));
                      warning.setFill(colorSpecFromRole(ColorRole::Fill1));
                      warning.setBorder(colorSpecFromRole(ColorRole::Accent), Style::borderWidth);
                    },
            },
            ui::label({
                .text = i18n::tr("settings.export-config.privacy-warning-title"),
                .fontSize = Style::fontSizeBody * m_scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::Label1),
                .maxLines = 0,
            }),
            ui::label({
                .text = i18n::tr("settings.export-config.privacy-warning-description"),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::Label1),
                .maxLines = 0,
            })
        )
    );

    root->addChild(std::move(body));

    const float buttonHeight = Tokens::sizeControlM * m_scale;
    root->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .justify = FlexJustify::End,
                .gap = Style::spaceSm * m_scale,
                .padding = inset,
            },
            ui::button({
                .text = i18n::tr("common.actions.cancel"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Default,
                .minHeight = buttonHeight,
                .paddingV = 0.0F,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick =
                    [this] {
                      const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                      DeferredCall::callLater([this, aliveGuard] {
                        if (!aliveGuard.expired()) {
                          close();
                        }
                      });
                    },
            }),
            ui::button({
                .out = &m_exportButton,
                .text = i18n::tr("settings.export-config.export"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Primary,
                .minHeight = buttonHeight,
                .paddingV = 0.0F,
                .paddingH = Style::spaceMd * m_scale,
                .radius = buttonHeight * 0.5F,
                .onClick = [this] {
                  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                  DeferredCall::callLater([this, aliveGuard] {
                    if (!aliveGuard.expired()) {
                      accept();
                    }
                  });
                },
            })
        )
    );

    return root;
  }

  LayoutSize ConfigExportDialogModal::measure(Renderer& renderer, const SettingsModalLayoutSpace& space) {
    if (m_root == nullptr) {
      return {.width = 1.0F, .height = 1.0F};
    }
    const float width = std::min(Tokens::componentDialogWidth * m_scale, space.maxContentWidth);
    LayoutConstraints constraints;
    constraints.setExactWidth(width);
    const float height = std::min(m_root->measure(renderer, constraints).height, space.maxContentHeight);
    return {.width = width, .height = height};
  }

  void ConfigExportDialogModal::arrange(Renderer& renderer, float width, float height) {
    if (m_root != nullptr) {
      m_root->arrange(renderer, {.x = 0.0F, .y = 0.0F, .width = width, .height = height});
    }
  }

  void ConfigExportDialogModal::setMode(ConfigExportMode mode) {
    m_mode = mode;
    if (m_mergedRadio != nullptr) {
      m_mergedRadio->setChecked(mode == ConfigExportMode::MergedUser);
    }
    if (m_fullRadio != nullptr) {
      m_fullRadio->setChecked(mode == ConfigExportMode::FullEffective);
    }
    requestRedraw();
  }

  void ConfigExportDialogModal::accept() {
    if (!m_open || m_host == nullptr || !m_modalId.has_value()) {
      return;
    }
    const ConfigExportMode mode = m_mode;
    ExportCallback callback = std::move(m_callback);
    if (!m_host->pop(*m_modalId)) {
      m_callback = std::move(callback);
      return;
    }
    if (callback) {
      callback(mode);
    }
  }

  std::unique_ptr<Flex>
  ConfigExportDialogModal::makeOption(ConfigExportMode mode, const std::string& title, const std::string& description) {
    RadioButton* radio = nullptr;
    auto option = ui::row(
        {
            .align = FlexAlign::Start,
            .gap = Style::spaceSm * m_scale,
            .padding = Style::spaceSm * m_scale,
            .fillWidth = true,
            .configure =
                [this](Flex& row) {
                  row.setRadius(Style::scaledRadiusMd(m_scale));
                  row.setFill(colorSpecFromRole(ColorRole::SolidThin));
                  row.setBorder(colorSpecFromRole(ColorRole::Stroke), Style::borderWidth);
                },
        },
        ui::radioButton({
            .out = &radio,
            .checked = m_mode == mode,
            .scale = m_scale,
            .onChange =
                [this, mode](bool checked) {
                  if (checked) {
                    setMode(mode);
                  }
                },
        }),
        ui::column(
            {
                .align = FlexAlign::Stretch,
                .gap = Style::spaceXs * m_scale,
                .flexGrow = 1.0F,
            },
            ui::label({
                .text = title,
                .fontSize = Style::fontSizeBody * m_scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::Label1),
                .maxLines = 1,
            }),
            ui::label({
                .text = description,
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::Label2),
                .maxWidth = (Tokens::componentDialogWidth - 92.0F) * m_scale,
                .maxLines = 3,
            })
        )
    );
    if (mode == ConfigExportMode::MergedUser) {
      m_mergedRadio = radio;
    } else {
      m_fullRadio = radio;
    }
    return option;
  }

  void ConfigExportDialogModal::requestLayout() {
    if (m_open && m_host != nullptr) {
      m_host->requestLayout();
    }
  }

  void ConfigExportDialogModal::requestRedraw() { requestLayout(); }

  void ConfigExportDialogModal::clearNodePointers() {
    m_root = nullptr;
    m_mergedRadio = nullptr;
    m_fullRadio = nullptr;
    m_exportButton = nullptr;
  }

} // namespace settings
