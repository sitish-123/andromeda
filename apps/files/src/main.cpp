#include "files_app.h"
#include "network_page.h"
#include "vaults.h"

#include <glib.h>

#include <string_view>

int main(int argc, char* argv[]) {
  bool picker = false;
  bool service = false; // D-Bus activation for org.freedesktop.FileManager1: no window until a request
  for (int i = 1; i < argc; ++i) {
    picker = picker || std::string_view(argv[i]) == "--picker";
    service = service || std::string_view(argv[i]) == "--service";
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--vault-guard" && i + 1 < argc) { // locks an unlocked vault before sleep
      return Vaults::guard(argv[i + 1]);
    }
    if (std::string_view(argv[i]) == "--reconnect") { // login: mount the saved servers marked "connect at login"
      GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
      int failed = 0;
      bool finished = false; // no saved server answers before the loop runs
      NetworkPage::reconnectSaved([&](int, int f) {
        failed = f;
        finished = true;
        g_main_loop_quit(loop);
      });
      if (!finished) {
        g_main_loop_run(loop);
      }
      g_main_loop_unref(loop);
      return failed == 0 ? 0 : 1;
    }
  }
  FilesApp app(picker, service);
  return app.run(argc, argv);
}
