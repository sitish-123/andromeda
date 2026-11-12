#include "output/mode_selection.h"

#include "wlr_color.h"

#include <cmath>

extern "C" {
#include <wlr/types/wlr_output.h>
}

namespace andromeda {

  bool outputCanAutoEnable(wlr_output* output) {
    if (wl_list_empty(&output->modes)) {
      return true;
    }
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->preferred) {
        return true;
      }
    }
    const auto populated = [](const char* value) { return value != nullptr && value[0] != '\0'; };
    return populated(output->make) || populated(output->model) || populated(output->serial);
  }

  wlr_output_mode* selectOutputMode(wlr_output* output, const OutputMode& configured) {
    wlr_output_mode* selected = nullptr;
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->width != configured.width || mode->height != configured.height) {
        continue;
      }
      if (configured.refreshMHz != 0) {
        if (selected == nullptr
            || std::abs(mode->refresh - configured.refreshMHz) < std::abs(selected->refresh - configured.refreshMHz)) {
          selected = mode;
        }
      } else if (
          selected == nullptr
          || (mode->preferred && !selected->preferred)
          || (mode->preferred == selected->preferred && mode->refresh > selected->refresh)
      ) {
        selected = mode;
      }
    }
    return selected;
  }

  wlr_output_mode* fastestPreferredMode(wlr_output* output, int capMHz) {
    wlr_output_mode* preferred = wlr_output_preferred_mode(output);
    if (preferred == nullptr) {
      return nullptr;
    }
    const auto allowed = [capMHz](const wlr_output_mode* m) { return capMHz <= 0 || m->refresh <= capMHz + 500; };
    wlr_output_mode* best = nullptr;
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->width == preferred->width && mode->height == preferred->height && allowed(mode)
          && (best == nullptr || mode->refresh > best->refresh)) {
        best = mode;
      }
    }
    return best != nullptr ? best : preferred;
  }

  wlr_output_mode* preferredFallbackMode(wlr_output* output, const wlr_output_mode* staged) {
    wlr_output_mode* preferred = wlr_output_preferred_mode(output);
    return preferred == staged ? nullptr : preferred;
  }

  wlr_output_mode* runningListedMode(wlr_output* output) {
    wlr_output_mode* mode = nullptr;
    wl_list_for_each(mode, &output->modes, link) {
      if (mode->width == output->width && mode->height == output->height && mode->refresh == output->refresh) {
        return mode;
      }
    }
    return nullptr;
  }

  bool liveModeMatches(const wlr_output& output, const wlr_output_mode* mode) {
    return output.enabled && output.current_mode == mode;
  }

  bool refreshOnlyChange(const wlr_output& output, const wlr_output_mode* mode) {
    const wlr_output_mode* live = output.current_mode;
    return output.enabled && live != nullptr && mode != nullptr && live != mode && live->width == mode->width
        && live->height == mode->height;
  }

  bool outputEnableNeedsStaging(const wlr_output& output, bool enabled) { return !(enabled && output.enabled); }

  bool colorChangeNeedsModeset(const wlr_output& output, uint32_t renderFormat, bool hdr) {
    return output.render_format != renderFormat || (output.image_description != nullptr) != hdr;
  }

} // namespace andromeda
