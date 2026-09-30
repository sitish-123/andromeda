#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "compositor/system_keyboard_config.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char* trim(char* value) {
  while (isspace((unsigned char)*value)) {
    value++;
  }
  char* end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  return value;
}

static bool copy_value(char* destination, size_t destination_size, const char* value) {
  const size_t length = strlen(value);
  if (length >= destination_size) {
    return false;
  }
  memcpy(destination, value, length + 1);
  return true;
}

static bool parse_assignment(char* line, char** key, char** value) {
  char* cursor = trim(line);
  if (cursor[0] == '\0' || cursor[0] == '#') {
    return false;
  }
  if (strncmp(cursor, "export", 6) == 0 && isspace((unsigned char)cursor[6])) {
    cursor = trim(cursor + 6);
  }

  char* equals = strchr(cursor, '=');
  if (equals == NULL) {
    return false;
  }
  *equals = '\0';
  *key = trim(cursor);
  cursor = trim(equals + 1);

  if (cursor[0] == '\'' || cursor[0] == '"') {
    const char quote = *cursor++;
    char* closing = strchr(cursor, quote);
    if (closing == NULL) {
      return false;
    }
    *closing = '\0';
    char* remainder = trim(closing + 1);
    if (remainder[0] != '\0' && remainder[0] != '#') {
      return false;
    }
    *value = cursor;
    return true;
  }

  char* comment = strchr(cursor, '#');
  if (comment != NULL) {
    *comment = '\0';
  }
  *value = trim(cursor);
  return true;
}

bool system_keyboard_config_load_file(const char* path, struct system_keyboard_config* config) {
  if (path == NULL || config == NULL) {
    return false;
  }

  FILE* file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }

  memset(config, 0, sizeof(*config));
  char* line = NULL;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) >= 0) {
    char* key = NULL;
    char* value = NULL;
    if (!parse_assignment(line, &key, &value)) {
      continue;
    }
    if (strcasecmp(key, "XKBMODEL") == 0 || strcasecmp(key, "XKB_MODEL") == 0) {
      copy_value(config->model, sizeof(config->model), value);
    } else if (strcasecmp(key, "XKBLAYOUT") == 0 || strcasecmp(key, "XKB_LAYOUT") == 0) {
      copy_value(config->layout, sizeof(config->layout), value);
    } else if (strcasecmp(key, "XKBVARIANT") == 0 || strcasecmp(key, "XKB_VARIANT") == 0) {
      copy_value(config->variant, sizeof(config->variant), value);
    } else if (strcasecmp(key, "XKBOPTIONS") == 0 || strcasecmp(key, "XKB_OPTIONS") == 0) {
      copy_value(config->options, sizeof(config->options), value);
    }
  }

  free(line);
  fclose(file);
  return config->layout[0] != '\0';
}

static bool parse_xorg_option(char* line, char** key, char** value) {
  char* cursor = trim(line);
  if (strlen(cursor) <= 6 || strncasecmp(cursor, "Option", 6) != 0 || !isspace((unsigned char)cursor[6])) {
    return false;
  }
  cursor = trim(cursor + 6);

  for (size_t field = 0; field < 2; ++field) {
    char** destination = field == 0 ? key : value;
    if (cursor[0] == '\'' || cursor[0] == '"') {
      const char quote = *cursor++;
      char* closing = strchr(cursor, quote);
      if (closing == NULL) {
        return false;
      }
      *closing = '\0';
      *destination = cursor;
      cursor = trim(closing + 1);
    } else {
      *destination = cursor;
      while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        cursor++;
      }
      if (*cursor != '\0') {
        *cursor++ = '\0';
        cursor = trim(cursor);
      }
    }
  }
  return (*key)[0] != '\0';
}

bool system_keyboard_config_load_xorg_file(const char* path, struct system_keyboard_config* config) {
  if (path == NULL || config == NULL) {
    return false;
  }

  FILE* file = fopen(path, "r");
  if (file == NULL) {
    return false;
  }

  memset(config, 0, sizeof(*config));
  char* line = NULL;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) >= 0) {
    char* key = NULL;
    char* value = NULL;
    if (!parse_xorg_option(line, &key, &value)) {
      continue;
    }
    if (strcasecmp(key, "XkbModel") == 0) {
      copy_value(config->model, sizeof(config->model), value);
    } else if (strcasecmp(key, "XkbLayout") == 0) {
      copy_value(config->layout, sizeof(config->layout), value);
    } else if (strcasecmp(key, "XkbVariant") == 0) {
      copy_value(config->variant, sizeof(config->variant), value);
    } else if (strcasecmp(key, "XkbOptions") == 0) {
      copy_value(config->options, sizeof(config->options), value);
    }
  }

  free(line);
  fclose(file);
  return config->layout[0] != '\0';
}

static int xorg_config_filter(const struct dirent* entry) {
  const size_t length = strlen(entry->d_name);
  return length > 5 && strcmp(entry->d_name + length - 5, ".conf") == 0;
}

static bool load_xorg_config_directory(
    const char* directory, struct system_keyboard_config* config, char* source_path, size_t source_path_size
) {
  struct dirent** entries = NULL;
  const int count = scandir(directory, &entries, xorg_config_filter, alphasort);
  if (count < 0) {
    return false;
  }

  bool found = false;
  for (int i = 0; i < count; ++i) {
    char path[PATH_MAX];
    const int length = snprintf(path, sizeof(path), "%s/%s", directory, entries[i]->d_name);
    if (length > 0 && (size_t)length < sizeof(path)) {
      struct system_keyboard_config candidate;
      if (system_keyboard_config_load_xorg_file(path, &candidate)) {
        *config = candidate;
        if (source_path != NULL && source_path_size > 0) {
          snprintf(source_path, source_path_size, "%s", path);
        }
        found = true;
      }
    }
    free(entries[i]);
  }
  free(entries);
  return found;
}

bool system_keyboard_config_load(struct system_keyboard_config* config, char* source_path, size_t source_path_size) {
  static const char* assignment_paths[] = {
      "/etc/default/keyboard",
      "/etc/vconsole.conf",
      "/etc/sysconfig/keyboard",
  };
  static const char* xorg_files[] = {
      "/etc/X11/xorg.conf",
  };
  static const char* xorg_directories[] = {
      "/etc/X11/xorg.conf.d",
      "/usr/local/share/X11/xorg.conf.d",
      "/usr/share/X11/xorg.conf.d",
  };

  if (config == NULL) {
    return false;
  }
  if (source_path != NULL && source_path_size > 0) {
    source_path[0] = '\0';
  }
  for (size_t i = 0; i < sizeof(assignment_paths) / sizeof(assignment_paths[0]); ++i) {
    if (system_keyboard_config_load_file(assignment_paths[i], config)) {
      if (source_path != NULL && source_path_size > 0) {
        snprintf(source_path, source_path_size, "%s", assignment_paths[i]);
      }
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(xorg_files) / sizeof(xorg_files[0]); ++i) {
    if (system_keyboard_config_load_xorg_file(xorg_files[i], config)) {
      if (source_path != NULL && source_path_size > 0) {
        snprintf(source_path, source_path_size, "%s", xorg_files[i]);
      }
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(xorg_directories) / sizeof(xorg_directories[0]); ++i) {
    if (load_xorg_config_directory(xorg_directories[i], config, source_path, source_path_size)) {
      return true;
    }
  }
  return false;
}
