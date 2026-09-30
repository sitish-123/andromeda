#pragma once

#include <stdbool.h>
#include <stddef.h>

struct system_keyboard_config {
  char model[128];
  char layout[128];
  char variant[128];
  char options[256];
};

bool system_keyboard_config_load_file(const char* path, struct system_keyboard_config* config);
bool system_keyboard_config_load_xorg_file(const char* path, struct system_keyboard_config* config);
bool system_keyboard_config_load(struct system_keyboard_config* config, char* source_path, size_t source_path_size);
