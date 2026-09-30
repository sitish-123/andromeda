#define _POSIX_C_SOURCE 200809L

#include "compositor/system_keyboard_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool expect_value(const char* name, const char* actual, const char* expected) {
  if (strcmp(actual, expected) == 0) {
    return true;
  }
  fprintf(stderr, "%s: expected '%s', got '%s'\n", name, expected, actual);
  return false;
}

int main(void) {
  char path[] = "/tmp/noctalia-keyboard-config-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    perror("mkstemp");
    return EXIT_FAILURE;
  }

  FILE* file = fdopen(fd, "w");
  if (file == NULL) {
    perror("fdopen");
    close(fd);
    unlink(path);
    return EXIT_FAILURE;
  }
  fputs(
      "# console-setup configuration\n"
      "XKBMODEL=\"apple\"\n"
      "export XKBLAYOUT = 'fr' # system layout\n"
      "XKBVARIANT=mac\n"
      "XKBOPTIONS=\"compose:ralt,terminate:ctrl_alt_bksp\"\n"
      "KEYMAP=fr-mac\n",
      file
  );
  if (fclose(file) != 0) {
    perror("fclose");
    unlink(path);
    return EXIT_FAILURE;
  }

  struct system_keyboard_config config;
  bool passed = system_keyboard_config_load_file(path, &config);
  passed = expect_value("model", config.model, "apple") && passed;
  passed = expect_value("layout", config.layout, "fr") && passed;
  passed = expect_value("variant", config.variant, "mac") && passed;
  passed = expect_value("options", config.options, "compose:ralt,terminate:ctrl_alt_bksp") && passed;

  file = fopen(path, "w");
  if (file == NULL) {
    perror("fopen");
    unlink(path);
    return EXIT_FAILURE;
  }
  fputs(
      "Section \"InputClass\"\n"
      "  Identifier \"system-keyboard\"\n"
      "  MatchIsKeyboard \"on\"\n"
      "  Option \"XkbModel\" \"pc105\"\n"
      "  Option \"XkbLayout\" \"de,fr\"\n"
      "  Option \"XkbVariant\" \",mac\"\n"
      "  Option \"XkbOptions\" \"grp:alt_shift_toggle\"\n"
      "EndSection\n",
      file
  );
  if (fclose(file) != 0) {
    perror("fclose");
    unlink(path);
    return EXIT_FAILURE;
  }

  passed = system_keyboard_config_load_xorg_file(path, &config) && passed;
  passed = expect_value("Xorg model", config.model, "pc105") && passed;
  passed = expect_value("Xorg layout", config.layout, "de,fr") && passed;
  passed = expect_value("Xorg variant", config.variant, ",mac") && passed;
  passed = expect_value("Xorg options", config.options, "grp:alt_shift_toggle") && passed;
  unlink(path);

  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
