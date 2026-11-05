#include "files_app.h"

#include <string_view>

int main(int argc, char* argv[]) {
  bool picker = false;
  for (int i = 1; i < argc; ++i) {
    picker = picker || std::string_view(argv[i]) == "--picker";
  }
  FilesApp app(picker);
  return app.run(argc, argv);
}
