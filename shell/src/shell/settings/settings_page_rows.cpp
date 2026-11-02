#include "shell/settings/settings_page_rows.h"

#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_selection_style.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

namespace settings::page_rows {

  std::unique_ptr<Flex> row(float scale) {
    return ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = metrics::kRowHeight * scale,
        .fillWidth = true,
    });
  }

  std::unique_ptr<Node> symbol(std::string_view glyph, float size, ColorRole role) {
    return ui::glyph({.glyph = std::string(glyph), .glyphSize = size, .color = colorSpecFromRole(role)});
  }

  std::unique_ptr<Node> plainButton(std::string text, ButtonVariant variant, float scale, std::function<void()> onClick) {
    return ui::button({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .variant = variant,
        .minHeight = Tokens::sizeControlS * scale,
        .paddingV = 0.0F,
        .paddingH = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onClick),
    });
  }

  std::unique_ptr<Node> iconButton(std::string_view glyph, std::string tooltip, float scale, std::function<void()> onClick) {
    return ui::button({
        .glyph = std::string(glyph),
        .glyphSize = Tokens::sizeSymbolInline * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .minWidth = Tokens::sizeControlS * scale,
        .minHeight = Tokens::sizeControlS * scale,
        .padding = 0.0F,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onClick),
    });
  }

  std::unique_ptr<Node> title(std::string text, float scale) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .flexGrow = 1.0F,
    });
  }

  void addHeader(Flex& content, std::string_view text, std::unique_ptr<Node> trailing, float scale) {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingH = Style::spaceLg * scale});
    header->addChild(
        makeLabel(text, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2), FontWeight::SemiBold)
    );
    if (trailing != nullptr) {
      header->addChild(std::move(trailing));
    }
    content.addChild(std::move(header));
  }

  Flex* addCard(Flex& content, float scale) { return addSettingsGroupCard(content, {}, scale); }

  void addValueRow(Flex& card, std::string_view label, std::string_view value, float scale) {
    auto r = row(scale);
    r->addChild(makeLabel(label, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1)));
    r->addChild(ui::spacer());
    r->addChild(makeLabel(value, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label2)));
    card.addChild(std::move(r));
  }

  void addEmptyState(Flex& content, std::string_view glyph, std::string_view text, float scale) {
    auto empty = ui::column(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .padding = Tokens::spaceP48 * scale, .fillWidth = true}
    );
    empty->addChild(symbol(glyph, 48.0F * scale, ColorRole::Label3));
    empty->addChild(makeLabel(text, Tokens::typeTitle3.size * scale, colorSpecFromRole(ColorRole::Label1),
                              FontWeight::SemiBold));
    content.addChild(std::move(empty));
  }

  void addHero(Flex& content, Hero hero, float scale) {
    constexpr float kIcon = 32.0F;
    Flex* card = addCard(content, scale);
    auto r = row(scale);
    r->setMinHeight(Tokens::spaceP48 * scale);
    r->addChild(ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(hero.on ? ColorRole::Accent : ColorRole::Fill2),
         .radius = kIcon * 0.5F * scale,
         .width = kIcon * scale,
         .height = kIcon * scale},
        symbol(hero.glyph, Tokens::sizeSymbol * scale, hero.on ? ColorRole::OnAccent : ColorRole::Label2)
    ));
    auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    copy->addChild(makeLabel(hero.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Label1),
                             FontWeight::SemiBold));
    if (!hero.status.empty()) {
      copy->addChild(makeLabel(hero.status, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Label2)));
    }
    r->addChild(std::move(copy));
    if (hero.action != nullptr) {
      r->addChild(std::move(hero.action));
    }
    if (hero.onToggle) {
      r->addChild(ui::toggle({.checked = hero.on, .scale = scale, .onChange = std::move(hero.onToggle)}));
    }
    card->addChild(std::move(r));
  }

} // namespace settings::page_rows
