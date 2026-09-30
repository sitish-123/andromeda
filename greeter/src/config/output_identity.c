#include "config/output_identity.h"

#include <stdio.h>
#include <string.h>

static bool nonempty(const char* value) { return value != NULL && value[0] != '\0'; }

static void append_component(char* out, size_t out_size, const char* component) {
  if (!nonempty(component) || out_size == 0) {
    return;
  }
  const size_t length = strlen(out);
  if (length >= out_size - 1) {
    return;
  }
  snprintf(out + length, out_size - length, "%s%s", length > 0 ? " " : "", component);
}

bool greeter_output_stable_identifier(
    const char* connector, const char* make, const char* model, const char* serial, const char* description, char* out,
    size_t out_size
) {
  if (out == NULL || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  if (nonempty(description)) {
    if (nonempty(connector)) {
      char suffix[512];
      const int suffix_length = snprintf(suffix, sizeof(suffix), " (%s)", connector);
      const size_t description_length = strlen(description);
      if (suffix_length > 0
          && (size_t)suffix_length < sizeof(suffix)
          && description_length > (size_t)suffix_length
          && strcmp(description + description_length - (size_t)suffix_length, suffix) == 0) {
        const size_t stable_length = description_length - (size_t)suffix_length;
        if (stable_length >= out_size) {
          return false;
        }
        memcpy(out, description, stable_length);
        out[stable_length] = '\0';
        return true;
      }
    }
    if (!nonempty(connector) || strcmp(description, connector) != 0) {
      const int written = snprintf(out, out_size, "%s", description);
      return written >= 0 && (size_t)written < out_size;
    }
  }

  append_component(out, out_size, make);
  append_component(out, out_size, model);
  append_component(out, out_size, serial);
  return out[0] != '\0';
}

bool greeter_output_identifier_matches(
    const char* connector, const char* make, const char* model, const char* serial, const char* description,
    const char* identifier
) {
  if (!nonempty(identifier)) {
    return false;
  }
  if (nonempty(connector) && strcmp(connector, identifier) == 0) {
    return true;
  }
  if (nonempty(description) && strcmp(description, identifier) == 0) {
    return true;
  }

  char stable[512];
  if (greeter_output_stable_identifier(connector, make, model, serial, description, stable, sizeof(stable))
      && strcmp(stable, identifier) == 0) {
    return true;
  }

  // wl_output geometry exposes make/model but not serial. Accept that pair so
  // the greeter client and compositor resolve the same identifier when the
  // protocol description omits EDID details. Full make/model/serial remains
  // preferred when available and unique.
  stable[0] = '\0';
  append_component(stable, sizeof(stable), make);
  append_component(stable, sizeof(stable), model);
  if (stable[0] != '\0' && strcmp(stable, identifier) == 0) {
    return true;
  }
  append_component(stable, sizeof(stable), serial);
  return stable[0] != '\0' && strcmp(stable, identifier) == 0;
}

bool greeter_output_name_is_internal(const char* connector) {
  return nonempty(connector) && (strncmp(connector, "eDP-", 4) == 0 || strncmp(connector, "LVDS-", 5) == 0);
}
