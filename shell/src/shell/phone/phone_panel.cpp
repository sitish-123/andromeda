#include "shell/phone/phone_panel.h"

#include "dbus/link/link_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "shell/phone/phone_state.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <memory>

namespace {

  constexpr float kActionHeight = 64.0F;
  // The actions' full names; their labels are short to fit four in a row.
  constexpr std::array kTooltips{
      "bar.widgets.phone.ring", "bar.widgets.phone.send-clipboard", "bar.widgets.phone.dnd", "bar.widgets.phone.ringer"
  };

} // namespace

float PhonePanel::measureContentHeight(Renderer& renderer, float contentWidth) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  m_rootLayout->setSize(contentWidth, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(contentWidth);
  return m_rootLayout->measure(renderer, constraints).height;
}

void PhonePanel::create() {
  const float scale = contentScale();
  auto layout = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale});
  layout->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
          ui::glyph({
              .out = &m_glyph,
              .glyph = "device-mobile",
              .glyphSize = Style::fontSizeTitle * 1.5F * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5F * scale, .flexGrow = 1.0F},
              ui::label({
                  .out = &m_name,
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
              }),
              ui::label({
                  .out = &m_state,
                  .fontSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
              })
          )
      )
  );
  auto actions = ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  for (std::uint8_t i = 0; i < ActionCount; ++i) {
    actions->addChild(
        ui::button({
            .out = &m_actions[i],
            .text = std::string(),
            .glyph = "bell",
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeTitle * scale,
            .contentAlign = ButtonContentAlign::Center,
            .surfaceOpacity = panelCardOpacity(),
            .tooltip = i18n::tr(kTooltips[i]),
            .minHeight = kActionHeight * scale,
            .gap = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .flexGrow = 1.0F,
            .onClick = [this, i]() { act(static_cast<Action>(i)); },
            .configure =
                [](Button& control) {
                  control.setDirection(FlexDirection::Vertical);
                  control.setAlign(FlexAlign::Center);
                  control.setJustify(FlexJustify::Center);
                },
        })
    );
  }
  layout->addChild(std::move(actions));
  layout->addChild(
      ui::button({
          .out = &m_open,
          .text = i18n::tr("bar.widgets.phone.open-link"),
          .glyph = "external-link",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = []() {
            LinkService::openApp();
            PanelManager::instance().close();
          },
      })
  );
  setRoot(std::move(layout));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void PhonePanel::onClose() {
  clearReleasedRoot();
  m_rootLayout = nullptr;
  m_glyph = nullptr;
  m_name = nullptr;
  m_state = nullptr;
  m_actions.fill(nullptr);
  m_open = nullptr;
}

void PhonePanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  for (Button* button : m_actions) {
    button->updateInputArea();
  }
  m_open->updateInputArea();
}

void PhonePanel::doUpdate(Renderer& /*renderer*/) {
  if (m_rootLayout == nullptr || m_link == nullptr) {
    return;
  }
  const LinkDevice* phone = phone::primary(*m_link);
  const LinkStatus* status = phone != nullptr ? m_link->status(phone->id) : nullptr;
  const LinkControls* controls = phone != nullptr && phone->connected ? m_link->controls(phone->id) : nullptr;
  const bool connected = phone != nullptr && phone->connected;
  const bool ringing = connected && m_link->phoneRinging(phone->id);

  if (phone == nullptr) {
    m_glyph->setGlyph("device-mobile-off");
    (void)m_name->setText(i18n::tr(m_link->available() ? "bar.widgets.phone.none" : "bar.widgets.phone.unavailable"));
    (void)m_state->setText({});
  } else {
    m_glyph->setGlyph(phone::glyph(*phone, status));
    (void)m_name->setText(phone->name);
    std::string state = phone::connectionText(*m_link, *phone);
    if (status != nullptr) {
      state = phone::batteryText(*status) + " · " + state;
    }
    (void)m_state->setText(state);
  }

  const std::string ringer = controls != nullptr ? controls->ringer : std::string();
  const auto set = [&](Action action, std::string_view glyph, const std::string& text, bool enabled, bool selected) {
    m_actions[action]->setGlyph(glyph);
    m_actions[action]->setText(text);
    m_actions[action]->setEnabled(enabled);
    m_actions[action]->setSelected(selected);
  };
  set(Ring, "bell-ringing", i18n::tr(ringing ? "bar.widgets.phone.stop-ring" : "bar.widgets.phone.ring"), connected,
      ringing);
  set(Clipboard, "clipboard-copy", i18n::tr("bar.widgets.phone.clipboard"), connected, false);
  set(Dnd, "moon", i18n::tr("bar.widgets.phone.dnd-short"), controls != nullptr && controls->dnd >= 0,
      controls != nullptr && controls->dnd == 1);
  set(Ringer, phone::ringerGlyph(ringer), i18n::tr("bar.widgets.phone.ringer-" + (ringer.empty() ? "normal" : ringer)),
      !ringer.empty(), false);
  m_rootLayout->markPaintDirty();
}

void PhonePanel::act(Action action) {
  const LinkDevice* phone = m_link != nullptr ? phone::primary(*m_link) : nullptr;
  if (phone == nullptr || !phone->connected) {
    return;
  }
  const LinkControls* controls = m_link->controls(phone->id);
  switch (action) {
  case Ring:
    m_link->ring(phone->id, !m_link->phoneRinging(phone->id));
    break;
  case Clipboard:
    m_link->shareClipboard(phone->id);
    break;
  case Dnd:
    if (controls != nullptr) {
      m_link->sendSystemCommand(phone->id, "set-dnd", controls->dnd == 1 ? 0 : 1);
    }
    break;
  case Ringer:
    if (controls != nullptr) {
      m_link->sendSystemCommand(phone->id, "set-ringer", phone::nextRinger(controls->ringer));
    }
    break;
  case ActionCount:
    break;
  }
}
