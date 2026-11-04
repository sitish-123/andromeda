#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Decodes systemd's \xNN escapes ("app-limine\x2dsnapper\x2dnotify@autostart.service").
[[nodiscard]] std::string unescapeUnitName(std::string_view name);

// systemd unit_name_escape(): keeps [A-Za-z0-9:_.], maps '/' to '-', writes \xNN for the rest and a leading '.'.
[[nodiscard]] std::string escapeUnitName(std::string_view text);

// Application ID of a desktop app unit, or "" when the name carries none. Accepts
// app[-<launcher>]-<AppID>[@<random>].service, app[-<launcher>]-<AppID>-<random>.scope (systemd.io/DESKTOP_ENVIRONMENTS),
// flatpak-<AppID>-<random>.scope and dbus-<bus>-<AppID>@<n>.service. An all-digit AppID (a PID) is rejected.
[[nodiscard]] std::string appIdFromUnit(std::string_view unit);

enum class UnitSlice : std::uint8_t { Other, App, Background, Session };

// Classifies by the last component of a Slice= value ("app-dbus\x2d....slice" counts as App).
[[nodiscard]] UnitSlice classifySlice(std::string_view slice);
