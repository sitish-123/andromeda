#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class AsyncTextureCache;
class Flex;
class Image;
class Label;

// Power menu (.agent/DESIGN.md "Power menu and session end", mockup power-menu.html section 0): who you are, the
// Lock / Sleep / Restart / Shut Down tiles, then Log Out across the row. Opens from the bar's power and brand
// buttons, Super+Esc and Ctrl+Alt+Del.
class SessionTab : public Tab {
public:
  explicit SessionTab(const ControlCenterServices& services);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] float measureHeight(Renderer& renderer, float width);

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  [[nodiscard]] std::unique_ptr<Flex> makeTile(std::string_view glyph, const std::string& label, std::string action,
                                               float height, bool roundIcon, Label** labelOut = nullptr);
  [[nodiscard]] std::unique_ptr<Flex> makeUpdateTile();
  [[nodiscard]] std::unique_ptr<Flex> makeBlockerRow();
  void run(const std::string& action);
  void syncWho(Renderer& renderer);
  void syncState();
  [[nodiscard]] bool updateOffered() const;

  ControlCenterServices m_services;
  Flex* m_rootLayout = nullptr;
  Image* m_avatar = nullptr;
  Label* m_subtitle = nullptr;
  std::string m_avatarPath;
  Flex* m_updateRow = nullptr;
  Label* m_updateLabel = nullptr;
  Flex* m_blockerRow = nullptr;
  Label* m_blockerTitle = nullptr;
  Label* m_blockerBody = nullptr;
  Label* m_restartLabel = nullptr;
  Label* m_shutdownLabel = nullptr;
  // Alt held: Restart becomes Restart to Firmware Setup (DESIGN.md).
  bool m_alt = false;
  bool m_watching = false;
};
