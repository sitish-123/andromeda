// andromeda-index-extract: reads one path per line on stdin and answers each with "<bytes>\n<text>", or "-\n" when the
// file cannot be read. Runs as a child of the index daemon, so poppler and libzip never load into the daemon and a
// crash in them costs only this process.
#include "extract.h"

#include <cstdio>
#include <iostream>
#include <string>

int main() {
  std::ios::sync_with_stdio(false);
  std::string path;
  while (std::getline(std::cin, path)) {
    const auto text = andromeda::index::extractText(path);
    if (text) {
      std::cout << text->size() << '\n';
      std::cout.write(text->data(), static_cast<std::streamsize>(text->size()));
    } else {
      std::cout << "-\n";
    }
    std::cout.flush();
  }
  return 0;
}
