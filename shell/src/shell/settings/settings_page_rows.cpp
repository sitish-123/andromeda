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

  std::unique_ptr<Node> valueText(std::string text, float scale) {
    return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale,
                      .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1});
  }

  std::unique_ptr<Flex> labelRow(std::string_view glyph, std::string label, float scale) {
    auto r = row(scale);
    r->setGap(Style::spaceMd * scale);
    if (!glyph.empty()) {
      r->addChild(symbol(glyph, Tokens::sizeSymbol * scale, ColorRole::Label2));
    }
    r->addChild(title(std::move(label), scale));
    return r;
  }

  std::unique_ptr<Node> chevronButton(std::string tooltip, float scale, std::function<void()> onClick) {
    return iconButton("chevron-right", std::move(tooltip), scale, std::move(onClick));
  }

  void addInfoRow(Flex& card, std::string_view glyph, std::string label, std::string value, float scale,
                  std::function<void()> onOpen) {
    auto r = labelRow(glyph, label, scale);
    if (!value.empty()) {
      r->addChild(valueText(std::move(value), scale));
    }
    if (onOpen) {
      r->addChild(chevronButton(std::move(label), scale, std::move(onOpen)));
    }
    card.addChild(std::move(r));
  }

  void addSwitchRow(Flex& card, std::string_view glyph, std::string label, bool on, float scale,
                    std::function<void(bool)> onChange) {
    auto r = labelRow(glyph, std::move(label), scale);
    r->addChild(ui::toggle({.checked = on, .scale = scale, .onChange = std::move(onChange)}));
    card.addChild(std::move(r));
  }

  Flex* addTwoLineRow(Flex& card, std::string_view glyph, std::string title, std::string subtitle, float scale) {
    auto r = row(scale);
    r->setGap(Style::spaceMd * scale);
    r->setMinHeight(Tokens::sizeRowTwo * scale);
    if (!glyph.empty()) {
      r->addChild(symbol(glyph, Tokens::sizeSymbol * scale, ColorRole::Label2));
    }
    auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    text->addChild(ui::label({.text = std::move(title), .fontSize = Style::fontSizeBody * scale,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
    if (!subtitle.empty()) {
      text->addChild(ui::label({.text = std::move(subtitle), .fontSize = Tokens::typeFootnote.size * scale,
                                .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1}));
    }
    r->addChild(std::move(text));
    return static_cast<Flex*>(card.addChild(std::move(r)));
  }

  void addActionRow(Flex& card, std::string text, bool indent, float scale, std::function<void()> onClick) {
    auto r = row(scale);
    if (indent) {
      r->addChild(ui::row({.width = Tokens::sizeSymbol * scale}));
      r->setGap(Style::spaceMd * scale);
    }
    auto button = ui::button({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .minHeight = Tokens::sizeControlS * scale,
        .padding = 0.0F,
        .onClick = std::move(onClick),
    });
    auto palette = Button::defaultPalette(ButtonVariant::Ghost);
    for (auto* state : {&palette.normal, &palette.hover, &palette.pressed}) {
      state->label = colorSpecFromRole(ColorRole::AccentText);
    }
    button->setCustomPalette(palette);
    button->setStateFillless(true);
    r->addChild(std::move(button));
    card.addChild(std::move(r));
  }

  void addFootnote(Flex& card, std::string text, float scale) {
    if (text.empty()) {
      return;
    }
    auto note = ui::row({.align = FlexAlign::Start, .paddingH = Style::spaceLg * scale, .fillWidth = true});
    note->addChild(ui::label({.text = std::move(text), .fontSize = Tokens::typeFootnote.size * scale,
                              .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 3, .flexGrow = 1.0F}));
    static_cast<Flex*>(card.parent())->addChild(std::move(note));
  }

  Flex* addHero(Flex& content, Hero hero, float scale) {
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
    return card;
  }

} // namespace settings::page_rows
