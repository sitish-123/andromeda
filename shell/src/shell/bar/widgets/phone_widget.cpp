#include "shell/bar/widgets/phone_widget.h"

#include "dbus/link/link_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  std::string networkLabel(const std::string& network) { return i18n::tr("bar.widgets.phone.network-" + network); }

} // namespace

PhoneWidget::PhoneWidget(LinkService* link) : m_link(link) {}

void PhoneWidget::create() {
  auto area = ui::inputArea({});
  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "device-mobile",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );
  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
      })
  );
  setRoot(std::move(area));
}

void PhoneWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || m_label == nullptr || rootNode == nullptr) {
    return;
  }
  syncState(renderer);
  m_glyph->measure(renderer);
  m_label->measure(renderer);
  float width = m_glyph->width();
  float height = m_glyph->height();
  if (m_label->width() > 0.0F) {
    height = std::max(height, m_label->height());
    m_label->setPosition(width + Style::spaceXs, std::round((height - m_label->height()) * 0.5F));
    width = m_label->x() + m_label->width();
  }
  m_glyph->setPosition(0.0F, std::round((height - m_glyph->height()) * 0.5F));
  rootNode->setSize(width, height);
}

void PhoneWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void PhoneWidget::syncState(Renderer& renderer) {
  auto* rootNode = root();
  if (m_glyph == nullptr || m_label == nullptr || rootNode == nullptr || m_link == nullptr) {
    return;
  }
  const auto& devices = m_link->devices();
  const auto phone = std::ranges::find_if(devices, [](const LinkDevice& device) { return device.connected; });
  const LinkStatus* status = phone != devices.end() ? m_link->status(phone->id) : nullptr;
  std::string key = phone == devices.end() ? std::string{} : phone->id + phone->name;
  if (status != nullptr) {
    key += std::format(" {} {} {}", status->battery, status->charging, status->network);
  }
  if (key == m_lastKey) {
    return;
  }
  m_lastKey = key;

  const bool show = phone != devices.end();
  if (rootNode->visible() != show || rootNode->participatesInLayout() != show) {
    rootNode->setVisible(show);
    rootNode->setParticipatesInLayout(show);
    requestUpdate();
  }
  if (!show) {
    static_cast<InputArea*>(rootNode)->clearTooltip();
    return;
  }

  const auto state = status != nullptr && status->charging ? BatteryState::Charging : BatteryState::Discharging;
  m_glyph->setGlyph(status != nullptr ? batteryGlyphName(status->battery, state) : "device-mobile");
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setText(status != nullptr ? std::format("{}%", status->battery) : std::string{});
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_label->measure(renderer);

  std::vector<TooltipRow> rows;
  if (status != nullptr) {
    std::string value = std::format("{}%", status->battery);
    if (status->charging) {
      value += ", " + i18n::tr("bar.widgets.phone.charging");
    }
    value += ", " + networkLabel(status->network);
    rows.push_back({phone->name, std::move(value)});
  } else {
    rows.push_back({phone->name, i18n::tr("control-center.devices.connected")});
  }
  static_cast<InputArea*>(rootNode)->setTooltip(std::move(rows));
  requestRedraw();
}
