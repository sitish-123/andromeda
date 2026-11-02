#pragma once

#include "shell/control_center/tab.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Button;
class Flex;
class Glyph;
class Image;
class Label;
class Node;
class Renderer;
class ScrollView;
class Slider;

namespace control_center {

  // One row of a grouped card (rulebook 9.7): symbol, title, optional second line, trailing value and symbols.
  struct DetailRow {
    std::string glyph;
    std::string title;
    std::string secondary;
    std::string trailing;
    std::vector<std::string> trailingGlyphs;
    bool selected = false; // lens
    bool enabled = true;
    std::function<void()> onClick;
  };

  // A row with a window slider (rulebook 9.6): a 24 app icon when `iconCandidates` is set, else a 16 symbol.
  struct DetailSliderRow {
    std::string glyph;
    std::vector<std::string> iconCandidates;
    std::string title;
    std::function<std::optional<float>()> value; // live level 0..1
    std::function<void(float)> onChange;
  };

  // A detail page of the control center sheet (rulebook 9.16): a scrolling list of grouped cards and a footer of
  // plain buttons. A subclass describes the content in build(); the page rebuilds it from doUpdate when
  // structureKey() changes or requestRebuild() ran, never inside a row callback.
  class DetailPage : public Tab {
  public:
    struct FooterButton {
      std::string text;
      std::function<void()> onClick;
    };

    void setOnBack(std::function<void()> onBack) { m_onBack = std::move(onBack); }
    std::unique_ptr<Flex> create() final;
    bool handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
    void onClose() override;
    // Natural height of the page at `width`: the whole list plus the footer.
    [[nodiscard]] float measureHeight(Renderer& renderer, float width);

  protected:
    // Changes whenever build() would produce a different structure.
    [[nodiscard]] virtual std::string structureKey() const = 0;
    virtual void build() = 0;
    [[nodiscard]] virtual std::vector<FooterButton> footerButtons() = 0;
    // Live values that do not change the structure (toggle position).
    virtual void sync() {}
    // A node above the list that survives rebuilds (a search field); null for none.
    virtual std::unique_ptr<Node> createTop() { return nullptr; }
    // True while a structure change alone must wait (typed text); requestRebuild() still rebuilds.
    [[nodiscard]] virtual bool deferRebuild() const { return false; }
    // Drops pointers into the header and body nodes the panel frees on close.
    virtual void releaseControls() {}

    void requestRebuild();
    // A section header over a fill_1 card; returns the card.
    Flex* addGroup(std::string_view header = {});
    void addGroupFooter(Flex& card, std::string_view text);
    Button* addRow(Flex& card, const DetailRow& row);
    void addSliderRow(Flex& card, const DetailSliderRow& row);
    // A plain row that shows the rest of a long list.
    void addShowAll(Flex& card, bool expanded, std::function<void()> toggle);
    void addEmpty(std::string_view glyph, std::string_view title, std::string_view body);
    // An inline error (rulebook 9.13): danger symbol, text, plain "Try Again".
    void addError(std::string_view text, std::function<void()> retry);

  private:
    struct SliderBinding {
      Slider* slider = nullptr;
      std::function<std::optional<float>()> value;
    };
    struct IconSlot {
      Image* image = nullptr;
      Glyph* fallback = nullptr;
      std::vector<std::string> candidates;
      bool done = false;
    };
    struct Ellipsized {
      Label* label = nullptr;
      float reserve = 0.0F;
    };

    void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
    void doUpdate(Renderer& renderer) override;
    void rebuild();
    void addSeparator(Flex& card);

    Flex* m_root = nullptr;
    ScrollView* m_scroll = nullptr;
    Flex* m_content = nullptr;
    Flex* m_footer = nullptr;
    Node* m_top = nullptr;
    Flex* m_rowCard = nullptr; // the card whose last row was added; a separator goes before the next row
    std::function<void()> m_onBack;
    std::string m_key;
    bool m_dirty = true;
    bool m_syncingSliders = false;
    std::vector<SliderBinding> m_sliders;
    std::vector<IconSlot> m_icons;
    std::vector<Ellipsized> m_ellipsized;
  };

} // namespace control_center
