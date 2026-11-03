#include "dbus/systemd/systemd_unit_name.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace {

  int hexValue(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  std::vector<std::string_view> splitDash(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
      const std::size_t dash = text.find('-', start);
      parts.push_back(text.substr(start, dash == std::string_view::npos ? std::string_view::npos : dash - start));
      if (dash == std::string_view::npos) {
        return parts;
      }
      start = dash + 1;
    }
  }

  bool allDigits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c) != 0; });
  }

  // The AppID is the last dash-separated part: an AppID's own dashes are escaped as \x2d.
  std::string lastPartAsAppId(std::string_view rest) {
    const auto parts = splitDash(rest);
    if (parts.empty() || parts.size() > 2 || parts.back().empty() || allDigits(parts.back())) {
      return {};
    }
    return unescapeUnitName(parts.back());
  }

} // namespace

std::string unescapeUnitName(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '\\' && i + 3 < name.size() && name[i + 1] == 'x') {
      const int hi = hexValue(name[i + 2]);
      const int lo = hexValue(name[i + 3]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 3;
        continue;
      }
    }
    out += name[i];
  }
  return out;
}

std::string escapeUnitName(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '/') {
      out += '-';
    } else if ((c == '.' && i == 0) || (std::isalnum(c) == 0 && c != ':' && c != '_' && c != '.')) {
      out += std::format("\\x{:02x}", c);
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string appIdFromUnit(std::string_view unit) {
  if (unit.ends_with(".service")) {
    unit.remove_suffix(8);
    const std::size_t at = unit.find('@');
    if (at != std::string_view::npos) {
      unit = unit.substr(0, at);
    }
    if (unit.starts_with("app-")) {
      return lastPartAsAppId(unit.substr(4));
    }
    if (unit.starts_with("dbus-")) {
      // dbus-<bus id>-<AppID>; the bus id (":1.2") has no dash.
      const std::size_t dash = unit.find('-', 5);
      return dash == std::string_view::npos ? std::string{} : lastPartAsAppId(unit.substr(dash + 1));
    }
    return {};
  }
  if (unit.ends_with(".scope")) {
    unit.remove_suffix(6);
    const std::size_t dash = unit.rfind('-');
    if (dash == std::string_view::npos) {
      return {};
    }
    unit = unit.substr(0, dash);
    if (unit.starts_with("app-")) {
      return lastPartAsAppId(unit.substr(4));
    }
    if (unit.starts_with("flatpak-")) {
      return lastPartAsAppId(unit.substr(8));
    }
  }
  return {};
}

UnitSlice classifySlice(std::string_view slice) {
  if (slice == "app.slice" || slice.starts_with("app-")) {
    return UnitSlice::App;
  }
  if (slice == "background.slice" || slice.starts_with("background-")) {
    return UnitSlice::Background;
  }
  if (slice == "session.slice" || slice.starts_with("session-")) {
    return UnitSlice::Session;
  }
  return UnitSlice::Other;
}
