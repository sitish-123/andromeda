#include "shell/bar/widgets/phone_widget.h"

#include "dbus/link/link_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/phone/phone_state.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

PhoneWidget::PhoneWidget(LinkService* link) : m_link(link) {}

void PhoneWidget::create() {
  auto area = ui::inputArea({});
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("phone"); });
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
  const LinkDevice* phone = phone::primary(*m_link);
  const LinkStatus* status = phone != nullptr ? m_link->status(phone->id) : nullptr;
  const LinkDeviceInfo* info = phone != nullptr ? m_link->info(phone->id) : nullptr;
  std::string key;
  if (phone != nullptr) {
    key = std::format("{} {} {}", phone->id, phone->name, phone->connected);
  }
  if (status != nullptr) {
    key += std::format(" {} {} {}", status->battery, status->charging, status->network);
  }
  if (info != nullptr) {
    key += std::format(" {} {}", info->transport, info->lastSeen);
  }
  if (key == m_lastKey) {
    return;
  }
  m_lastKey = key;

  const bool show = phone != nullptr;
  if (rootNode->visible() != show || rootNode->participatesInLayout() != show) {
    rootNode->setVisible(show);
    rootNode->setParticipatesInLayout(show);
    requestUpdate();
  }
  if (!show) {
    static_cast<InputArea*>(rootNode)->clearTooltip();
    return;
  }

  m_glyph->setGlyph(phone::glyph(*phone, status));
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(
      widgetIconColorOr(colorSpecFromRole(phone->connected ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant))
  );
  m_label->setText(status != nullptr ? std::format("{}%", status->battery) : std::string{});
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_label->measure(renderer);

  std::string value = phone::connectionText(*m_link, *phone);
  if (status != nullptr) {
    value = phone::batteryText(*status) + ", " + value;
  }
  static_cast<InputArea*>(rootNode)->setTooltip(std::vector<TooltipRow>{{phone->name, std::move(value)}});
  requestRedraw();
}
