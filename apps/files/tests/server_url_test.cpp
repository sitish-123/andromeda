#include "server_url.h"

#include <print>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "server_url_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  auto smb = parseServerUrl(" smb://nas.local/Media Share ");
  expect(smb && smb->host == "nas.local" && smb->path == "/Media Share", "smb host and share");
  expect(smb && smb->uri() == "smb://nas.local/Media%20Share", "smb uri encodes the space");

  auto sftp = parseServerUrl("sftp://ann@build-box:2222/home/ann");
  expect(sftp && sftp->scheme == "sftp" && sftp->user == "ann" && sftp->host == "build-box" && sftp->port == 2222 &&
             sftp->path == "/home/ann",
         "sftp user, host, port and path");
  expect(sftp && sftp->uri() == "sftp://ann@build-box:2222/home/ann", "sftp uri round trip");

  auto dav = parseServerUrl("webdav://cloud.example.com/remote.php/dav");
  expect(dav && dav->scheme == "dav" && dav->port == 0, "webdav maps to dav");
  auto davs = parseServerUrl("davs://[fe80::1]:8443");
  expect(davs && davs->scheme == "davs" && davs->host == "[fe80::1]" && davs->port == 8443 && davs->path == "/",
         "davs with an IPv6 host");

  auto bare = parseServerUrl("\\\\fileserver\\public");
  expect(bare && bare->uri() == "smb://fileserver/public", "a UNC path is smb");

  expect(!parseServerUrl("gopher://host"), "an unknown scheme is refused");
  expect(!parseServerUrl("smb://"), "no host is refused");
  expect(!parseServerUrl("sftp://host:99999/"), "a port above 65535 is refused");
  expect(!parseServerUrl("sftp://ann:secret@host/"), "a password in the address is refused");
  expect(!parseServerUrl("smb://bad host/x"), "a space in the host is refused");
  return g_failures == 0 ? 0 : 1;
}
