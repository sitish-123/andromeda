// Standalone driver for net::openInBrowser, used by url_open_native.sh to prove it never spawns xdg-open.
#include "net/url_open.h"

#include <cstdio>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: url-open-test <url>\n");
    return 2;
  }
  const bool launched = net::openInBrowser(argv[1]);
  std::printf("launched=%d\n", launched ? 1 : 0);
  return 0;
}
