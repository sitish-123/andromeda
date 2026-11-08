#pragma once

#include "ui/controls/button.h"
#include "ui/palette.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Flex;
class Node;

// Parts of the hand-built Settings pages (Wi-Fi, Bluetooth, Network, Sound): rows 40, headers on the text line,
// plain small buttons, hero card (rulebook 9.7, direction A in .agent/DESIGN.md).
namespace settings::page_rows {

  [[nodiscard]] std::unique_ptr<Flex> row(float scale);
  [[nodiscard]] std::unique_ptr<Node> symbol(std::string_view glyph, float size, ColorRole role);
  [[nodiscard]] std::unique_ptr<Node> plainButton(std::string text, ButtonVariant variant, float scale,
                                                  std::function<void()> onClick);
  [[nodiscard]] std::unique_ptr<Node> iconButton(std::string_view glyph, std::string tooltip, float scale,
                                                 std::function<void()> onClick);
  [[nodiscard]] std::unique_ptr<Node> title(std::string text, float scale);
  void addHeader(Flex& content, std::string_view text, std::unique_ptr<Node> trailing, float scale);
  [[nodiscard]] Flex* addCard(Flex& content, float scale);
  void addValueRow(Flex& card, std::string_view label, std::string_view value, float scale);
  void addEmptyState(Flex& content, std::string_view glyph, std::string_view text, float scale);

  // D6 rows (.agent/mockups/settings-d6.html): grey leading symbol (none when empty), label_1 title, label_2 value.
  [[nodiscard]] std::unique_ptr<Node> valueText(std::string text, float scale);
  [[nodiscard]] std::unique_ptr<Flex> labelRow(std::string_view glyph, std::string label, float scale);
  // A value row; with onOpen the row ends in a chevron that opens it.
  void addInfoRow(Flex& card, std::string_view glyph, std::string label, std::string value, float scale,
                  std::function<void()> onOpen = {});
  void addSwitchRow(Flex& card, std::string_view glyph, std::string label, bool on, float scale,
                    std::function<void(bool)> onChange);
  // Two lines (title, label_2 subtitle) at the 56 row height; returns the row for trailing controls.
  [[nodiscard]] Flex* addTwoLineRow(Flex& card, std::string_view glyph, std::string title, std::string subtitle,
                                    float scale);
  // Accent text row ("Add Rule…") aligned with the titles of rows that have a symbol.
  void addActionRow(Flex& card, std::string text, bool indent, float scale, std::function<void()> onClick);
  // Footnote under a card of addSettingsGroupCard, inside its group (8 below the card).
  void addFootnote(Flex& card, std::string text, float scale);
  [[nodiscard]] std::unique_ptr<Node> chevronButton(std::string tooltip, float scale, std::function<void()> onClick);

  struct Hero {
    std::string glyph;
    bool on = false;
    std::string title;
    std::string status;
    // Shown before the toggle, e.g. "Details…".
    std::unique_ptr<Node> action;
    // No toggle when empty.
    std::function<void(bool)> onToggle;
  };
  // Returns the hero's card, for rows that belong with it (a progress bar).
  Flex* addHero(Flex& content, Hero hero, float scale);

} // namespace settings::page_rows
