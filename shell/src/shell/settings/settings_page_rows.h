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
  void addHero(Flex& content, Hero hero, float scale);

} // namespace settings::page_rows
