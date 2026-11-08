#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// A Connect to Server address. Typed forms: smb://host/share, sftp://user@host:port/path, dav(s)://host/path,
// nfs://host/export, ftp://host, \\host\share (smb) and a bare host[/path] (smb). A password in the address is
// refused: it goes in the Password field, so it never lands in recent servers.
struct ServerUrl {
  std::string scheme; // smb, sftp, dav, davs, nfs, ftp
  std::string user;
  std::string host;   // an IPv6 address keeps its brackets
  std::uint16_t port = 0;
  std::string path;   // starts with '/', as typed (not percent-encoded)

  // The GIO URI: path percent-encoded, user and port only when set.
  [[nodiscard]] std::string uri() const;
  // "host" or "user@host", for lists.
  [[nodiscard]] std::string label() const;
};

// The error is a sentence for the sheet.
[[nodiscard]] std::expected<ServerUrl, std::string> parseServerUrl(std::string_view input);
