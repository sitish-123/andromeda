// LD_PRELOAD into Umbriel: fails the next N frame commits (a buffer, no modeset) on output $FAIL_COMMIT_OUTPUT, N read
// from and counted down in $FAIL_COMMIT_FILE, standing in for a DRM commit that returns EBUSY. Logs every other commit
// it passes on to stderr as "commit-shim: <output> committed=<wlr_output_state_field mask>".
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>

bool wlr_output_commit_state(struct wlr_output* output, const struct wlr_output_state* state) {
  static bool (*real)(struct wlr_output*, const struct wlr_output_state*);
  if (real == NULL) {
    real = (bool (*)(struct wlr_output*, const struct wlr_output_state*))dlsym(RTLD_NEXT, "wlr_output_commit_state");
  }
  const char* name = getenv("FAIL_COMMIT_OUTPUT");
  const char* path = getenv("FAIL_COMMIT_FILE");
  if (name != NULL && path != NULL && strcmp(output->name, name) == 0 && (state->committed & WLR_OUTPUT_STATE_BUFFER)
      && !(state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_TRANSFORM))) {
    int pending = 0;
    FILE* file = fopen(path, "r");
    if (file != NULL) {
      if (fscanf(file, "%d", &pending) != 1) {
        pending = 0;
      }
      fclose(file);
    }
    if (pending > 0 && (file = fopen(path, "w")) != NULL) {
      fprintf(file, "%d\n", pending - 1);
      fclose(file);
      return false;
    }
  }
  fprintf(stderr, "commit-shim: %s committed=%#x\n", output->name, state->committed);
  return real(output, state);
}
