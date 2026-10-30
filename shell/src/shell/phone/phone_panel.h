#pragma once

#include "shell/panel/panel.h"

#include <array>
#include <cstdint>
#include <string>

class Button;
class Flex;
class Glyph;
class Label;
class LinkService;

// The bar phone item's popover: the phone's name, battery, and connection or last-seen time; Ring, Send clipboard, Do
// not disturb, and Ringer mode; and Open Link.
class PhonePanel : public Panel {
public:
  explicit PhonePanel(LinkService* link) : m_link(link) {}

  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(440.0F); }
  // Fits its content; this is the maximum.
  [[nodiscard]] float preferredHeight() const override { return scaled(320.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Attached; }

private:
  enum Action : std::uint8_t { Ring, Clipboard, Dnd, Ringer, ActionCount };

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void act(Action action);

  LinkService* m_link = nullptr;
  Flex* m_rootLayout = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_name = nullptr;
  Label* m_state = nullptr;
  std::array<Button*, ActionCount> m_actions{};
  Button* m_open = nullptr;
};
