#include "shell/control_center/detail/detail_page.h"

#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "system/icon_resolver.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/builders.h"
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
    constexpr float kEmptyTitleLines = 3.0F;

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

    m_top = nullptr;
    if (auto top = createTop(); top != nullptr) {
      m_top = top.get();
      root->addChild(std::move(top));
    }

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
    const float top = m_top != nullptr ? m_top->measure(renderer, constraints).height + m_root->gap() : 0.0F;
    return top + m_content->measure(renderer, constraints).height + m_root->gap() + m_footer->measure(renderer, constraints).height;
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
    m_top = nullptr;
    m_root = nullptr;
    m_scroll = nullptr;
    m_content = nullptr;
    m_sliders.clear();
    m_icons.clear();
    m_ellipsized.clear();
  }

  void DetailPage::setFooterVisible(bool visible) {
    if (m_footer != nullptr) {
      m_footer->setVisible(visible);
    }
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
      const float width = std::max(0.0F, contentWidth - item.reserve);
      item.label->setMaxWidth(item.cap > 0.0F ? std::min(width, item.cap) : width);
    }
    for (IconSlot& slot : m_icons) {
      if (slot.done) {
        continue;
      }
      slot.done = true;
      const int px = slot.px > 0 ? slot.px : static_cast<int>(std::ceil(Tokens::spaceP24 * contentScale()));
      bool loaded = false;
      if (slot.raw != nullptr && slot.raw->width > 0 && slot.raw->height > 0 && !slot.raw->data.empty()
          && slot.raw->bitsPerSample == 8 && (slot.raw->channels == 3 || slot.raw->channels == 4)) {
        const PixmapFormat format = slot.raw->channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
        loaded = slot.image->setSourceRaw(renderer, slot.raw->data.data(), slot.raw->data.size(), slot.raw->width,
                                          slot.raw->height, slot.raw->rowStride, format, true);
      }
      for (std::size_t i = 0; !loaded && i < slot.candidates.size(); ++i) {
        const std::string& candidate = slot.candidates[i];
        const std::string path = !candidate.empty() && candidate.front() == '/' ? candidate : iconResolver().resolve(candidate, px);
        loaded = !path.empty() && slot.image->setSourceFile(renderer, path, px, true);
      }
      if (loaded) {
        slot.image->setVisible(true);
        if (slot.fallback != nullptr) {
          slot.fallback->setVisible(false);
        }
      } else if (slot.frame != nullptr) {
        slot.frame->setVisible(false);
      }
    }
    m_root->setSize(contentWidth, bodyHeight);
    m_root->layout(renderer);
  }

  Flex* DetailPage::addGroup(std::string_view header, std::string_view action, std::function<void()> onAction) {
    Flex* card = addGroup(header);
    auto* section = static_cast<Flex*>(card->parent());
    if (section == nullptr || section->children().size() < 2) {
      return card;
    }
    auto* inset = static_cast<Flex*>(section->children().front().get());
    const float s = contentScale();
    inset->setDirection(FlexDirection::Horizontal);
    inset->setAlign(FlexAlign::Center);
    inset->setPadding(0.0F, Tokens::componentListCardPadding * s, 0.0F,
                      (Tokens::componentListCardPadding + Tokens::componentListRowInset) * s);
    if (!inset->children().empty()) {
      inset->children().front()->setFlexGrow(1.0F);
    }
    auto button = std::make_unique<Button>();
    button->setVariant(ButtonVariant::Default);
    button->setText(std::string(action));
    button->setControlHeight(Tokens::spaceP24 * s);
    button->setRadius(Tokens::spaceP8 * s);
    button->setOnClick(std::move(onAction));
    inset->addChild(std::move(button));
    return card;
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
    card->setFill(colorSpecFromRole(ColorRole::GlassCard, panelCardOpacity()));
    card->setBorder(colorSpecFromRole(ColorRole::GlassCardEdge), 0.5F * s);
    Flex* raw = card.get();
    section->addChild(std::move(card));
    m_content->addChild(std::move(section));
    return raw;
  }

  void DetailPage::bindImage(Image* image, std::vector<std::string> candidates, int decodePx,
                             std::shared_ptr<const NotificationImageData> raw) {
    m_icons.push_back({.image = image, .candidates = std::move(candidates), .raw = std::move(raw), .px = decodePx});
  }

  void DetailPage::addBlock(std::unique_ptr<Node> node) {
    m_rowCard = nullptr;
    m_content->addChild(std::move(node));
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
    const ColorSpec baseFill = colorSpecFromRole(ColorRole::Fill1, 0.0F);
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
      button->setOnEnter([cellPtr] { cellPtr->setFill(colorSpecFromRole(ColorRole::Fill1)); });
      button->setOnLeave([cellPtr] { cellPtr->setFill(colorSpecFromRole(ColorRole::Fill1, 0.0F)); });
    }

    float reserve = 2.0F * (Tokens::componentListCardPadding + Tokens::componentListRowInset);
    if (row.avatarImage != nullptr || !row.avatarIcons.empty()) {
      const float box = Tokens::spaceP24 * s;
      auto frame = std::make_unique<Flex>();
      frame->setMinWidth(box);
      frame->setMinHeight(box);
      frame->setRadius(box * 0.5F);
      auto image = std::make_unique<Image>();
      image->setFit(ImageFit::Cover);
      image->setSize(box, box);
      image->setRadius(box * 0.5F);
      image->setParticipatesInLayout(false);
      image->setVisible(false);
      m_icons.push_back({.image = image.get(), .candidates = row.avatarIcons, .raw = row.avatarImage, .frame = frame.get()});
      frame->addChild(std::move(image));
      button->addChild(std::move(frame));
      reserve += Tokens::spaceP24 + Tokens::spaceP8;
    } else if (!row.glyph.empty() && row.selected) {
      // The current item is marked by an accent round icon, not a row fill (rulebook 9.16).
      const float box = Tokens::spaceP24 * s;
      auto badge = std::make_unique<Flex>();
      badge->setAlign(FlexAlign::Center);
      badge->setJustify(FlexJustify::Center);
      badge->setMinWidth(box);
      badge->setMinHeight(box);
      badge->setRadius(box * 0.5F);
      badge->setFill(colorSpecFromRole(ColorRole::Accent));
      badge->addChild(glyph(row.glyph, Tokens::sizeSymbol * 0.75F * s, ColorRole::OnAccent));
      button->addChild(std::move(badge));
      reserve += Tokens::spaceP24 + Tokens::spaceP8;
    } else if (!row.glyph.empty()) {
      button->addChild(glyph(row.glyph, Tokens::sizeSymbol * s, ColorRole::Label2));
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
    // Beside the row button, not inside it: the row's input area covers its whole frame and would take the click.
    std::unique_ptr<Button> action;
    if (!row.action.empty()) {
      action = std::make_unique<Button>();
      action->setVariant(ButtonVariant::Default);
      action->setText(row.action);
      action->setFontSize(Tokens::typeFootnote.size * s);
      action->setControlHeight(Tokens::sizeControlS * s);
      action->setRadius(Tokens::radiusR8 * s);
      action->setPadding(0.0F, Tokens::spaceP8 * s, 0.0F, Tokens::spaceP8 * s);
      action->setOnClick(row.onAction);
      // App-given labels have any length: the button caps and ellipsizes, and the title reserves the cap plus inset.
      action->setMaxWidth(Tokens::sizeRow * 3 * s);
      trailingWidth += Tokens::sizeRow * 3 + Tokens::componentListRowInset;
    }
    std::unique_ptr<Toggle> toggle;
    if (row.toggle.has_value()) {
      toggle = ui::toggle({
          .checkedImmediate = *row.toggle,
          .toggleSize = ToggleSize::Medium,
          .scale = s,
          .onChange = row.onToggle,
      });
      toggle->setEnabled(row.toggleEnabled);
      trailingWidth += Tokens::sizeRow + Tokens::componentListRowInset;
    }
    // Reserve for the scrollbar gutter too.
    reserve += trailingWidth + Tokens::spaceP12;
    m_ellipsized.push_back({.label = title, .reserve = reserve * s});
    if (second != nullptr) {
      m_ellipsized.push_back({.label = second, .reserve = reserve * s});
    }
    Button* raw = button.get();
    cell->addChild(std::move(button));
    if (action != nullptr) {
      auto slot = std::make_unique<Flex>();
      slot->setAlign(FlexAlign::Center);
      slot->setJustify(FlexJustify::Center);
      slot->setPadding(0.0F, Tokens::componentListRowInset * s, 0.0F, 0.0F);
      slot->addChild(std::move(action));
      cell->addChild(std::move(slot));
    }
    if (toggle != nullptr) {
      auto slot = std::make_unique<Flex>();
      slot->setAlign(FlexAlign::Center);
      slot->setJustify(FlexJustify::Center);
      slot->setPadding(0.0F, Tokens::componentListRowInset * s, 0.0F, 0.0F);
      slot->addChild(std::move(toggle));
      cell->addChild(std::move(slot));
    }
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
        m_syncingSliders = true;
        slider->setValue(*value);
        m_syncingSliders = false;
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
    heading->setMaxLines(static_cast<int>(kEmptyTitleLines));
    heading->setMaxWidth(kEmptyBodyMaxWidth * s);
    heading->setTextAlign(TextAlign::Center);
    auto gap2 = std::make_unique<Flex>();
    gap2->setMinHeight(Tokens::spaceP4 * s);
    column->addChild(std::move(gap2));
    Label* text = nullptr;
    auto bodyLabel = label(text, Tokens::typeBody, s, ColorRole::Label2, body);
    text->setMaxLines(static_cast<int>(kEmptyBodyLines));
    text->setMaxWidth(kEmptyBodyMaxWidth * s);
    text->setTextAlign(TextAlign::Center);
    // Both lines wrap inside the page at any scale: 320 is a cap, not a fixed width.
    for (Label* line : {heading, text}) {
      m_ellipsized.push_back({.label = line, .reserve = 2.0F * Tokens::spaceP12 * s, .cap = kEmptyBodyMaxWidth * s});
    }
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
