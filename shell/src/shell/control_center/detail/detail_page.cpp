#include "shell/control_center/detail/detail_page.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "system/icon_resolver.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace control_center {

  namespace {

    constexpr float kEmptyBodyMaxWidth = 320.0F; // rulebook 9.13
    constexpr float kEmptyBodyLines = 2.0F;

    // Function-local: the icon theme walk must not run before main().
    IconResolver& iconResolver() {
      static IconResolver resolver;
      return resolver;
    }

    std::unique_ptr<Label> label(Label*& out, const Tokens::Type& type, float scale, ColorRole role, std::string_view text, int weight = 0) {
      auto l = std::make_unique<Label>();
      l->setFontSize(type.size * scale);
      l->setLineHeight(type.line * scale);
      l->setFontWeight(static_cast<FontWeight>(Style::glassWeight(weight > 0 ? weight : type.weight)));
      l->setColor(colorSpecFromRole(role));
      l->setMaxLines(1);
      l->setEllipsize(TextEllipsize::End);
      l->setText(std::string(text));
      out = l.get();
      return l;
    }

    std::unique_ptr<Glyph> glyph(std::string_view name, float size, ColorRole role) {
      auto g = std::make_unique<Glyph>();
      g->setGlyph(name);
      g->setGlyphSize(size);
      g->setColor(colorSpecFromRole(role));
      return g;
    }

    void clearChildren(Node& node) {
      std::vector<Node*> doomed;
      for (const auto& child : node.children()) {
        doomed.push_back(child.get());
      }
      for (Node* child : doomed) {
        node.removeChild(child);
      }
    }

  } // namespace

  std::unique_ptr<Flex> DetailPage::create() {
    const float s = contentScale();
    m_key.clear();
    m_dirty = true;
    m_sliders.clear();
    m_icons.clear();
    m_ellipsized.clear();

    auto root = std::make_unique<Flex>();
    m_root = root.get();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Stretch);
    root->setGap(Tokens::spaceP8 * s);

    auto scroll = std::make_unique<ScrollView>();
    scroll->setOrientation(ScrollOrientation::Vertical);
    scroll->setContentScale(s);
    scroll->setViewportPaddingH(0.0F);
    scroll->setViewportPaddingV(0.0F);
    scroll->clearFill();
    scroll->clearBorder();
    scroll->setFlexGrow(1.0F);
    m_scroll = scroll.get();
    m_content = scroll->content();
    m_content->setDirection(FlexDirection::Vertical);
    m_content->setAlign(FlexAlign::Stretch);
    m_content->setGap(Tokens::componentListGroupGap * s);
    root->addChild(std::move(scroll));

    auto footer = std::make_unique<Flex>();
    footer->setDirection(FlexDirection::Horizontal);
    footer->setAlign(FlexAlign::Center);
    footer->setGap(Tokens::componentButtonGap * s);
    for (FooterButton& item : footerButtons()) {
      auto button = std::make_unique<Button>();
      button->setVariant(ButtonVariant::Ghost);
      button->setText(item.text);
      button->setFontSize(Tokens::typeFootnote.size * s);
      button->setControlHeight(Tokens::componentButtonHeightS * s);
      button->setRadius(Tokens::componentButtonRadius * s);
      button->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
      button->setOnClick(std::move(item.onClick));
      footer->addChild(std::move(button));
    }
    m_footer = footer.get();
    root->addChild(std::move(footer));
    return root;
  }

  float DetailPage::measureHeight(Renderer& renderer, float width) {
    if (m_root == nullptr) {
      return 0.0F;
    }
    LayoutConstraints constraints;
    constraints.setExactWidth(width);
    return m_content->measure(renderer, constraints).height + m_root->gap() + m_footer->measure(renderer, constraints).height;
  }

  bool DetailPage::handleKey(std::uint32_t sym, std::uint32_t /*modifiers*/, bool pressed, bool preedit) {
    if (!pressed || preedit || sym != XKB_KEY_Escape || !m_onBack) {
      return false;
    }
    m_onBack();
    return true;
  }

  void DetailPage::onClose() {
    releaseControls();
    m_footer = nullptr;
    m_root = nullptr;
    m_scroll = nullptr;
    m_content = nullptr;
    m_sliders.clear();
    m_icons.clear();
    m_ellipsized.clear();
  }

  void DetailPage::requestRebuild() {
    m_dirty = true;
    PanelManager::instance().refresh();
  }

  void DetailPage::rebuild() {
    m_key = structureKey();
    m_dirty = false;
    m_sliders.clear();
    m_icons.clear();
    m_ellipsized.clear();
    clearChildren(*m_content);
    m_rowCard = nullptr;
    build();
    PanelManager::instance().requestLayout();
  }

  void DetailPage::doUpdate(Renderer& /*renderer*/) {
    if (m_content == nullptr) {
      return;
    }
    // A rebuild frees the dragged Slider; it waits for the release.
    const bool dragging = std::ranges::any_of(m_sliders, [](const SliderBinding& b) { return b.slider->dragging(); });
    if (!dragging && (m_dirty || (!deferRebuild() && structureKey() != m_key))) {
      rebuild();
    }
    m_syncingSliders = true;
    for (const SliderBinding& binding : m_sliders) {
      if (binding.slider->dragging()) {
        continue;
      }
      if (const std::optional<float> value = binding.value(); value.has_value()) {
        binding.slider->setValue(*value);
      }
    }
    m_syncingSliders = false;
    sync();
  }

  void DetailPage::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
    if (m_root == nullptr) {
      return;
    }
    for (const Ellipsized& item : m_ellipsized) {
      item.label->setMaxWidth(std::max(0.0F, contentWidth - item.reserve));
    }
    m_root->setSize(contentWidth, bodyHeight);
    m_root->layout(renderer);
    for (IconSlot& slot : m_icons) {
      if (slot.done) {
        continue;
      }
      slot.done = true;
      const int px = static_cast<int>(std::ceil(Tokens::spaceP24 * contentScale()));
      for (const std::string& candidate : slot.candidates) {
        const std::string& path = iconResolver().resolve(candidate, px);
        if (!path.empty() && slot.image->setSourceFile(renderer, path, px, true)) {
          slot.image->setVisible(true);
          slot.fallback->setVisible(false);
          break;
        }
      }
    }
  }

  Flex* DetailPage::addGroup(std::string_view header) {
    const float s = contentScale();
    auto section = std::make_unique<Flex>();
    section->setDirection(FlexDirection::Vertical);
    section->setAlign(FlexAlign::Stretch);
    section->setGap(Tokens::componentListHeaderGap * s);
    if (!header.empty()) {
      Label* title = nullptr;
      auto text = label(title, Tokens::typeFootnote, s, ColorRole::Label2, header, 600);
      auto inset = std::make_unique<Flex>();
      inset->setPadding(0.0F, 0.0F, 0.0F, (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
      inset->addChild(std::move(text));
      section->addChild(std::move(inset));
    }
    auto card = std::make_unique<Flex>();
    card->setDirection(FlexDirection::Vertical);
    card->setAlign(FlexAlign::Stretch);
    card->setPadding(Tokens::componentListCardPadding * s);
    card->setRadius(Tokens::componentListCardRadius * s);
    card->setFill(colorSpecFromRole(ColorRole::Fill1, panelCardOpacity()));
    Flex* raw = card.get();
    section->addChild(std::move(card));
    m_content->addChild(std::move(section));
    return raw;
  }

  void DetailPage::addGroupFooter(Flex& card, std::string_view text) {
    const float s = contentScale();
    Label* note = nullptr;
    auto line = label(note, Tokens::typeFootnote, s, ColorRole::Label2, text);
    auto inset = std::make_unique<Flex>();
    inset->setPadding(0.0F, 0.0F, 0.0F, (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
    inset->addChild(std::move(line));
    static_cast<Flex*>(card.parent())->addChild(std::move(inset));
  }

  void DetailPage::addSeparator(Flex& card) {
    const float s = contentScale();
    auto line = std::make_unique<Flex>();
    line->setMinHeight(1.0F);
    line->setFill(colorSpecFromRole(ColorRole::Separator));
    auto inset = std::make_unique<Flex>();
    inset->setDirection(FlexDirection::Vertical);
    inset->setAlign(FlexAlign::Stretch);
    inset->setPadding(0.0F, 0.0F, 0.0F, Tokens::componentListRowInset * s);
    inset->addChild(std::move(line));
    card.addChild(std::move(inset));
  }

  Button* DetailPage::addRow(Flex& card, const DetailRow& row) {
    const float s = contentScale();
    if (m_rowCard == &card) {
      addSeparator(card);
    }
    m_rowCard = &card;
    const bool twoLine = !row.secondary.empty();
    const ColorSpec baseFill = row.selected ? colorSpecFromRole(ColorRole::Lens)
                                            : colorSpecFromRole(ColorRole::Fill1, 0.0F);
    auto cell = std::make_unique<Flex>();
    cell->setDirection(FlexDirection::Horizontal);
    cell->setAlign(FlexAlign::Stretch);
    cell->setRadius(Tokens::componentListRowRadius * s);
    cell->setFill(baseFill);
    Flex* cellPtr = cell.get();

    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Ghost);
    button->setStateFillless(true);
    button->setDirection(FlexDirection::Horizontal);
    button->setAlign(FlexAlign::Center);
    button->setContentAlign(ButtonContentAlign::Start);
    button->setMinHeight((twoLine ? Tokens::sizeRowTwo : Tokens::sizeRow) * s);
    button->setPadding(0.0F, Tokens::componentListRowInset * s, 0.0F, Tokens::componentListRowInset * s);
    button->setGap(Tokens::spaceP8 * s);
    button->setRadius(Tokens::componentListRowRadius * s);
    button->setFlexGrow(1.0F);
    button->setEnabled(row.enabled && static_cast<bool>(row.onClick));
    if (row.onClick) {
      button->setOnClick(row.onClick);
      if (!row.selected) {
        button->setOnEnter([cellPtr] { cellPtr->setFill(colorSpecFromRole(ColorRole::Fill2)); });
        button->setOnLeave([cellPtr] { cellPtr->setFill(colorSpecFromRole(ColorRole::Fill1, 0.0F)); });
      }
    }

    float reserve = 2.0F * (Tokens::componentListCardPadding + Tokens::componentListRowInset);
    if (!row.glyph.empty()) {
      button->addChild(glyph(row.glyph, Tokens::sizeSymbol * s, row.selected ? ColorRole::AccentText : ColorRole::Label2));
      reserve += Tokens::sizeSymbol + Tokens::spaceP8;
    }
    auto text = std::make_unique<Flex>();
    text->setDirection(FlexDirection::Vertical);
    text->setAlign(FlexAlign::Start);
    text->setFlexGrow(1.0F);
    Label* title = nullptr;
    text->addChild(label(title, Tokens::typeBody, s, row.enabled ? ColorRole::Label1 : ColorRole::Label3, row.title));
    Label* second = nullptr;
    if (twoLine) {
      text->addChild(label(second, Tokens::typeFootnote, s, ColorRole::Label2, row.secondary));
    }
    button->addChild(std::move(text));
    float trailingWidth = 0.0F;
    if (!row.trailing.empty()) {
      Label* value = nullptr;
      button->addChild(label(value, Tokens::typeBody, s, ColorRole::Label2, row.trailing));
      trailingWidth += 48.0F;
    }
    for (const std::string& name : row.trailingGlyphs) {
      button->addChild(glyph(name, Tokens::sizeSymbol * s, ColorRole::Label2));
      trailingWidth += Tokens::sizeSymbol + Tokens::spaceP8;
    }
    // Reserve for the scrollbar gutter too.
    reserve += trailingWidth + Tokens::spaceP12;
    m_ellipsized.push_back({.label = title, .reserve = reserve * s});
    if (second != nullptr) {
      m_ellipsized.push_back({.label = second, .reserve = reserve * s});
    }
    Button* raw = button.get();
    cell->addChild(std::move(button));
    card.addChild(std::move(cell));
    return raw;
  }

  void DetailPage::addSliderRow(Flex& card, const DetailSliderRow& row) {
    const float s = contentScale();
    if (m_rowCard == &card) {
      addSeparator(card);
    }
    m_rowCard = &card;
    auto cell = std::make_unique<Flex>();
    cell->setDirection(FlexDirection::Vertical);
    cell->setAlign(FlexAlign::Stretch);
    cell->setMinHeight(Tokens::sizeRowTwo * s);
    cell->setPadding(Tokens::spaceP4 * s, Tokens::componentListRowInset * s, Tokens::spaceP4 * s,
                     Tokens::componentListRowInset * s);

    auto top = std::make_unique<Flex>();
    top->setDirection(FlexDirection::Horizontal);
    top->setAlign(FlexAlign::Center);
    top->setGap(Tokens::spaceP8 * s);
    float reserve = 2.0F * (Tokens::componentListCardPadding + Tokens::componentListRowInset) + Tokens::spaceP12;
    if (!row.iconCandidates.empty()) {
      const float box = Tokens::spaceP24 * s;
      auto frame = std::make_unique<Flex>();
      frame->setDirection(FlexDirection::Horizontal);
      frame->setAlign(FlexAlign::Center);
      frame->setJustify(FlexJustify::Center);
      frame->setMinWidth(box);
      frame->setMinHeight(box);
      auto fallback = glyph(row.glyph.empty() ? "volume" : row.glyph, Tokens::sizeSymbol * s, ColorRole::Label2);
      auto image = std::make_unique<Image>();
      image->setFit(ImageFit::Cover);
      image->setSize(box, box);
      image->setParticipatesInLayout(false);
      image->setVisible(false);
      m_icons.push_back({.image = image.get(), .fallback = fallback.get(), .candidates = row.iconCandidates});
      frame->addChild(std::move(fallback));
      frame->addChild(std::move(image));
      top->addChild(std::move(frame));
      reserve += Tokens::spaceP24 + Tokens::spaceP8;
    } else if (!row.glyph.empty()) {
      top->addChild(glyph(row.glyph, Tokens::sizeSymbol * s, ColorRole::Label2));
      reserve += Tokens::sizeSymbol + Tokens::spaceP8;
    }
    Label* title = nullptr;
    top->addChild(label(title, Tokens::typeBody, s, ColorRole::Label1, row.title));
    m_ellipsized.push_back({.label = title, .reserve = reserve * s});
    cell->addChild(std::move(top));

    auto slider = std::make_unique<Slider>();
    slider->setRange(0.0, 1.0);
    slider->setStep(0.01);
    slider->setControlHeight(Tokens::sizeControlS * s);
    Slider* raw = slider.get();
    // A rebuild held back by the drag runs after the release.
    slider->setOnDragEnd([] { PanelManager::instance().refresh(); });
    slider->setOnValueChanged([this, onChange = row.onChange](double value) {
      if (!m_syncingSliders && onChange) {
        onChange(static_cast<float>(value));
      }
    });
    if (row.value) {
      if (const std::optional<float> value = row.value(); value.has_value()) {
        slider->setValue(*value);
      }
      m_sliders.push_back({.slider = raw, .value = row.value});
    }
    cell->addChild(std::move(slider));
    card.addChild(std::move(cell));
  }

  void DetailPage::addShowAll(Flex& card, bool expanded, std::function<void()> toggle) {
    DetailRow row;
    row.title = i18n::tr(expanded ? "control-center.detail.show-fewer" : "control-center.detail.show-all");
    row.trailingGlyphs = {expanded ? "chevron-up" : "chevron-down"};
    row.onClick = std::move(toggle);
    addRow(card, row);
  }

  void DetailPage::addEmpty(std::string_view glyphName, std::string_view title, std::string_view body) {
    const float s = contentScale();
    auto column = std::make_unique<Flex>();
    column->setDirection(FlexDirection::Vertical);
    column->setAlign(FlexAlign::Center);
    column->setJustify(FlexJustify::Center);
    column->setPadding(Tokens::spaceP40 * s, 0.0F);
    column->addChild(glyph(glyphName, Tokens::spaceP48 * s, ColorRole::Label3));
    auto gap1 = std::make_unique<Flex>();
    gap1->setMinHeight(Tokens::spaceP16 * s);
    column->addChild(std::move(gap1));
    Label* heading = nullptr;
    column->addChild(label(heading, Tokens::typeTitle3, s, ColorRole::Label1, title));
    auto gap2 = std::make_unique<Flex>();
    gap2->setMinHeight(Tokens::spaceP4 * s);
    column->addChild(std::move(gap2));
    Label* text = nullptr;
    auto bodyLabel = label(text, Tokens::typeBody, s, ColorRole::Label2, body);
    text->setMaxLines(static_cast<int>(kEmptyBodyLines));
    text->setMaxWidth(kEmptyBodyMaxWidth * s);
    text->setTextAlign(TextAlign::Center);
    column->addChild(std::move(bodyLabel));
    m_content->addChild(std::move(column));
  }

  void DetailPage::addError(std::string_view text, std::function<void()> retry) {
    const float s = contentScale();
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Tokens::spaceP8 * s);
    row->addChild(glyph("alert-circle", Tokens::sizeSymbol * s, ColorRole::DangerText));
    Label* message = nullptr;
    auto messageLabel = label(message, Tokens::typeFootnote, s, ColorRole::Label1, text);
    messageLabel->setFlexGrow(1.0F);
    row->addChild(std::move(messageLabel));
    auto again = std::make_unique<Button>();
    again->setVariant(ButtonVariant::Ghost);
    again->setText(i18n::tr("control-center.detail.try-again"));
    again->setFontSize(Tokens::typeFootnote.size * s);
    again->setControlHeight(Tokens::componentButtonHeightS * s);
    again->setRadius(Tokens::componentButtonRadius * s);
    again->setPadding(0.0F, Tokens::componentButtonInsetS * s, 0.0F, Tokens::componentButtonInsetS * s);
    again->setOnClick(std::move(retry));
    row->addChild(std::move(again));
    m_content->addChild(std::move(row));
  }

} // namespace control_center
