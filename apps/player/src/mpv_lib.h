#pragma once

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

// libmpv loaded with dlopen on first use, so the player starts (and the package installs) without it; only the
// functions the player calls are resolved.
struct MpvLib {
  decltype(&mpv_client_api_version) clientApiVersion = nullptr;
  decltype(&mpv_create) create = nullptr;
  decltype(&mpv_initialize) initialize = nullptr;
  decltype(&mpv_terminate_destroy) terminateDestroy = nullptr;
  decltype(&mpv_set_option_string) setOptionString = nullptr;
  decltype(&mpv_command) command = nullptr;
  decltype(&mpv_command_async) commandAsync = nullptr;
  decltype(&mpv_set_property) setProperty = nullptr;
  decltype(&mpv_set_property_string) setPropertyString = nullptr;
  decltype(&mpv_get_property) getProperty = nullptr;
  decltype(&mpv_observe_property) observeProperty = nullptr;
  decltype(&mpv_wait_event) waitEvent = nullptr;
  decltype(&mpv_set_wakeup_callback) setWakeupCallback = nullptr;
  decltype(&mpv_error_string) errorString = nullptr;
  decltype(&mpv_free) free = nullptr;
  decltype(&mpv_render_context_create) renderCreate = nullptr;
  decltype(&mpv_render_context_set_update_callback) renderSetUpdateCallback = nullptr;
  decltype(&mpv_render_context_update) renderUpdate = nullptr;
  decltype(&mpv_render_context_render) render = nullptr;
  decltype(&mpv_render_context_report_swap) renderReportSwap = nullptr;
  decltype(&mpv_render_context_free) renderFree = nullptr;

  // Null when libmpv.so.2 is missing or lacks a symbol; loads once per process.
  [[nodiscard]] static const MpvLib* get();
};
