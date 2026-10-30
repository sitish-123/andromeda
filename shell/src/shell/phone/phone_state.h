#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class LinkService;
struct LinkDevice;
struct LinkStatus;

// What the bar item, its popover, the quick settings tile, and the lock screen say about a Link phone.
namespace phone {

  // The phone the shell shows: the first connected one, else the first paired one.
  [[nodiscard]] const LinkDevice* primary(const LinkService& link);
  // Wi-Fi or Bluetooth while connected, else when the phone was last seen.
  [[nodiscard]] std::string connectionText(const LinkService& link, const LinkDevice& device);
  // "42%", or "42%, charging".
  [[nodiscard]] std::string batteryText(const LinkStatus& status);
  [[nodiscard]] std::string glyph(const LinkDevice& device, const LinkStatus* status);
  // The set-ringer value that follows `ringer` in the cycle normal, vibrate, silent.
  [[nodiscard]] std::uint32_t nextRinger(std::string_view ringer);
  [[nodiscard]] std::string ringerGlyph(std::string_view ringer);

} // namespace phone
