#pragma once

#include "shell/lockscreen/lock_notice_groups.h"
#include "ui/auth/auth_layout.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Glyph;
class Image;
class Label;
class LinkService;
class MprisService;
class Node;
class NotificationManager;
class RectNode;
class Renderer;

// Rulebook 9.15 cards and widgets on the lock screen: the media card, the chips row (Centered), the card column
// (Glance) and the small-widget row (Split), in the slots that placeAuth() returns. Draws and hit-tests only.
class LockCards {
public:
  struct Media {
    std::string busName;
    std::string title;
    // "artist · app"
    std::string subtitle;
    std::string artUrl;
    // artUrl resolved to a local file by the host; empty = music symbol.
    std::string artPath;
    bool playing = false;
    bool operator==(const Media&) const = default;
  };

  struct Phone {
    std::string name;
    std::string glyph;
    std::uint32_t battery = 0;
    // Notifications waiting on the phone (count only).
    std::uint32_t notices = 0;
    bool operator==(const Phone&) const = default;
  };

  struct Data {
    std::optional<Media> media;
    std::vector<LockNoticeGroup> groups;
    std::optional<Phone> phone;
    // Charging chip text; empty while not charging.
    std::string charging;
    bool operator==(const Data&) const = default;
  };

  enum class Control { None, Previous, PlayPause, Next };

  // `parent` = the layer that recedes with the Glance sheet (AuthColumn::glanceLayer()).
  void build(Node& parent);
  void setData(Data data);
  [[nodiscard]] const Data& data() const noexcept { return m_data; }
  // Placement input for this data in `layout`.
  void fillPlacementInput(AuthPlacementInput& in) const;
  void layout(Renderer& renderer, const AuthPlacement& place, float scale);
  [[nodiscard]] Control hitTest(float x, float y) const;

private:
  struct Card {
    RectNode* bg = nullptr;
    Image* image = nullptr;
    RectNode* tile = nullptr;
    Glyph* glyph = nullptr;
    Label* title = nullptr;
    Label* titleRest = nullptr;
    Label* body = nullptr;
    Label* trailing = nullptr;
    std::string imagePath;
  };

  struct Chip {
    RectNode* bg = nullptr;
    Label* label = nullptr;
  };

  struct Widget {
    RectNode* bg = nullptr;
    Label* value = nullptr;
    Label* label = nullptr;
  };

  struct Button {
    RectNode* bg = nullptr;
    Glyph* glyph = nullptr;
    AuthRect rect;
  };

  [[nodiscard]] std::vector<std::string> chipTexts() const;
  [[nodiscard]] int glanceCardCount() const;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> widgets() const;
  void layoutMediaCard(Renderer& renderer, Card& card, const AuthRect& rect, float s);
  void layoutNoticeCard(Renderer& renderer, Card& card, const LockNoticeGroup& group, const AuthRect& rect, float s);
  void layoutPhoneCard(Renderer& renderer, Card& card, const AuthRect& rect, float s);
  void hideAll();

  Data m_data;
  Node* m_group = nullptr;
  std::vector<Card> m_cards;
  std::vector<Chip> m_chips;
  std::vector<Widget> m_widgets;
  std::array<Button, 3> m_buttons{};
  bool m_buttonsShown = false;
};

// Lock card data from the shell services; any service may be null. Notification content never enters it.
[[nodiscard]] LockCards::Data collectLockCards(
    const MprisService* mpris, const LinkService* link, const NotificationManager* notifications
);
