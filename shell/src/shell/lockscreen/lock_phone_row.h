#pragma once

#include <memory>
#include <string>

class Flex;
class Glyph;
class Label;
class LinkService;
class MprisService;
class Renderer;

// The lock screen's phone row, at the bottom-left corner: the Link phone's battery, what its player plays, and how
// many of its notifications wait. Hidden while no phone is connected.
class LockPhoneRow {
public:
  [[nodiscard]] std::unique_ptr<Flex> create();
  void setServices(const LinkService* link);
  // Update phase: copies the phone's state, and its player from `mpris`, into the row.
  void sync(const MprisService* mpris);
  // Layout phase: places the row, or hides it when `shown` is false or there is nothing to show.
  void layout(Renderer& renderer, float screenWidth, float screenHeight, bool shown);

private:
  const LinkService* m_link = nullptr;
  Flex* m_row = nullptr;
  Glyph* m_battery = nullptr;
  Label* m_phone = nullptr;
  Glyph* m_playerGlyph = nullptr;
  Label* m_player = nullptr;
  Glyph* m_countGlyph = nullptr;
  Label* m_count = nullptr;
  bool m_hasPhone = false;
};
