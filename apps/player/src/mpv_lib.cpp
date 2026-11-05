#include "mpv_lib.h"

#include "core/log.h"

#include <dlfcn.h>

namespace {
  constexpr Logger kLog("mpv");

  template <typename T> bool resolve(void* handle, const char* name, T& out) {
    out = reinterpret_cast<T>(::dlsym(handle, name));
    if (out == nullptr) {
      kLog.warn("libmpv lacks {}", name);
    }
    return out != nullptr;
  }

  MpvLib* load() {
    void* handle = ::dlopen("libmpv.so.2", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      kLog.warn("cannot load libmpv.so.2: {}", ::dlerror());
      return nullptr;
    }
    static MpvLib lib;
    const bool ok = resolve(handle, "mpv_client_api_version", lib.clientApiVersion)
        && resolve(handle, "mpv_create", lib.create) && resolve(handle, "mpv_initialize", lib.initialize)
        && resolve(handle, "mpv_terminate_destroy", lib.terminateDestroy)
        && resolve(handle, "mpv_set_option_string", lib.setOptionString) && resolve(handle, "mpv_command", lib.command)
        && resolve(handle, "mpv_command_async", lib.commandAsync) && resolve(handle, "mpv_set_property", lib.setProperty)
        && resolve(handle, "mpv_set_property_string", lib.setPropertyString)
        && resolve(handle, "mpv_get_property", lib.getProperty)
        && resolve(handle, "mpv_observe_property", lib.observeProperty) && resolve(handle, "mpv_wait_event", lib.waitEvent)
        && resolve(handle, "mpv_set_wakeup_callback", lib.setWakeupCallback)
        && resolve(handle, "mpv_error_string", lib.errorString) && resolve(handle, "mpv_free", lib.free)
        && resolve(handle, "mpv_render_context_create", lib.renderCreate)
        && resolve(handle, "mpv_render_context_set_update_callback", lib.renderSetUpdateCallback)
        && resolve(handle, "mpv_render_context_update", lib.renderUpdate)
        && resolve(handle, "mpv_render_context_render", lib.render)
        && resolve(handle, "mpv_render_context_report_swap", lib.renderReportSwap)
        && resolve(handle, "mpv_render_context_free", lib.renderFree);
    if (!ok) {
      ::dlclose(handle);
      return nullptr;
    }
    // The handle stays open for the process: mpv threads may outlive any owner here.
    return &lib;
  }
} // namespace

const MpvLib* MpvLib::get() {
  static const MpvLib* lib = load();
  return lib;
}
