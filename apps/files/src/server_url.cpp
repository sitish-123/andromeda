#include "server_url.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Typed aliases map to the GIO scheme.
std::string gioScheme(std::string_view typed) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kSchemes{{
      {"smb", "smb"}, {"cifs", "smb"}, {"sftp", "sftp"}, {"ssh", "sftp"}, {"dav", "dav"},
      {"webdav", "dav"}, {"davs", "davs"}, {"webdavs", "davs"}, {"nfs", "nfs"},
  }};
  const std::string s = lower(typed);
  if (s == "ftp") {
    return "ftp";
  }
  for (const auto& [from, to] : kSchemes) {
    if (s == from) {
      return std::string(to);
    }
  }
  return {};
}

bool hostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_';
}

bool userChar(char c) { return hostChar(c) || c == '~' || c == '%' || c == '+' || c == ';'; }

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

} // namespace

std::expected<ServerUrl, std::string> parseServerUrl(std::string_view input) {
  std::string_view s = trim(input);
  if (s.empty()) {
    return std::unexpected("Type a server address, for example smb://nas.local/share.");
  }
  std::string unc;
  ServerUrl url;
  if (s.starts_with("\\\\")) {
    unc = std::string(s.substr(2));
    std::ranges::replace(unc, '\\', '/');
    s = unc;
    url.scheme = "smb";
  } else if (const auto sep = s.find("://"); sep != std::string_view::npos) {
    url.scheme = gioScheme(s.substr(0, sep));
    if (url.scheme.empty()) {
      return std::unexpected("Files cannot connect with \"" + std::string(s.substr(0, sep)) +
                             "\". Use smb, sftp, dav, davs, nfs or ftp.");
    }
    s.remove_prefix(sep + 3);
  } else {
    url.scheme = "smb";
  }

  const auto slash = s.find('/');
  std::string_view authority = s.substr(0, slash);
  url.path = slash == std::string_view::npos ? "/" : std::string(s.substr(slash));

  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    const std::string_view user = authority.substr(0, at);
    if (user.find(':') != std::string_view::npos) {
      return std::unexpected("Type the password in the Password field, not in the address.");
    }
    if (user.empty() || !std::ranges::all_of(user, userChar)) {
      return std::unexpected("The user name in the address is not valid.");
    }
    url.user = std::string(user);
    authority.remove_prefix(at + 1);
  }

  std::string_view portText;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1 ||
        !std::ranges::all_of(authority.substr(1, close - 1), [](char c) { return hex(c) || c == ':' || c == '.'; })) {
      return std::unexpected("The IPv6 address is not valid.");
    }
    url.host = std::string(authority.substr(0, close + 1));
    const std::string_view rest = authority.substr(close + 1);
    if (!rest.empty() && !rest.starts_with(':')) {
      return std::unexpected("The server address is not valid.");
    }
    portText = rest.empty() ? rest : rest.substr(1);
    if (!rest.empty() && portText.empty()) {
      return std::unexpected("Type a port number after the colon.");
    }
  } else {
    const auto colon = authority.find(':');
    url.host = std::string(authority.substr(0, colon));
    if (colon != std::string_view::npos) {
      portText = authority.substr(colon + 1);
      if (portText.empty()) {
        return std::unexpected("Type a port number after the colon.");
      }
    }
    if (url.host.empty()) {
      return std::unexpected("Type the server name or address.");
    }
    if (!std::ranges::all_of(url.host, hostChar) || url.host.starts_with('-') || url.host.starts_with('.')) {
      return std::unexpected("\"" + url.host + "\" is not a valid server name.");
    }
  }

  if (!portText.empty()) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), value);
    if (ec != std::errc{} || end != portText.data() + portText.size() || value == 0 || value > 65535) {
      return std::unexpected("The port must be a number from 1 to 65535.");
    }
    url.port = static_cast<std::uint16_t>(value);
  }
  return url;
}

std::string ServerUrl::uri() const {
  std::string out = scheme + "://";
  if (!user.empty()) {
    out += user + "@";
  }
  out += host;
  if (port != 0) {
    out += ":" + std::to_string(port);
  }
  constexpr std::string_view kHex = "0123456789ABCDEF";
  for (std::size_t i = 0; i < path.size(); ++i) {
    const auto c = static_cast<unsigned char>(path[i]);
    const bool escaped = c == '%' && i + 2 < path.size() && hex(path[i + 1]) && hex(path[i + 2]);
    if (escaped || hostChar(static_cast<char>(c)) || c == '/' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4U];
      out += kHex[c & 0xfU];
    }
  }
  return out;
}

std::string ServerUrl::label() const { return user.empty() ? host : user + "@" + host; }
