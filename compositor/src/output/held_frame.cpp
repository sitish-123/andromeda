#include "output/held_frame.h"

#include "core/log.h"
#include "wlr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
}

namespace andromeda {
  namespace {

    constexpr Logger kLog("held-frame");

    // Rulebook 7.2: close (d 0.28, b 0) and fade_out (d 0.10, b 0) as stiffness k = (2 pi / d)^2, damping ratio 1.
    constexpr desk::motion::SpringConfig kClose{.damping = 1.0, .stiffness = 504.0, .mass = 1.0};
    constexpr desk::motion::SpringConfig kFadeOut{.damping = 1.0, .stiffness = 3948.0, .mass = 1.0};
    constexpr double kExitScale = 0.02;

    struct HeldBuffer {
      wlr_buffer base;
      wlr_dmabuf_attributes dmabuf;
    };

    HeldBuffer* fromBuffer(wlr_buffer* buffer) {
      return reinterpret_cast<HeldBuffer*>(reinterpret_cast<char*>(buffer) - offsetof(HeldBuffer, base));
    }

    bool heldGetDmabuf(wlr_buffer* buffer, wlr_dmabuf_attributes* dmabuf) {
      *dmabuf = fromBuffer(buffer)->dmabuf;
      return true;
    }

    void heldDestroy(wlr_buffer* buffer) {
      HeldBuffer* held = fromBuffer(buffer);
      wlr_buffer_finish(buffer);
      wlr_dmabuf_attributes_finish(&held->dmabuf);
      delete held;
    }

    const wlr_buffer_impl kHeldBufferImpl = {
        .destroy = heldDestroy,
        .get_dmabuf = heldGetDmabuf,
        .get_shm = nullptr,
        .begin_data_ptr_access = nullptr,
        .end_data_ptr_access = nullptr,
    };

#if ANDROMEDA_HAS_NATIVE_DRM_POLICY
    // GETFB2 on the CRTC that drives this connector, exported as dmabuf fds. GEM handles are closed again: the fds keep
    // the buffer alive after the next commit replaces it on the CRTC.
    wlr_buffer* exportCrtcFrame(wlr_output* output) {
      if (!wlr_output_is_drm(output)) {
        return nullptr;
      }
      const int fd = wlr_backend_get_drm_fd(output->backend);
      if (fd < 0) {
        return nullptr;
      }
      drmModeConnector* connector = drmModeGetConnector(fd, wlr_drm_connector_get_id(output));
      if (connector == nullptr) {
        return nullptr;
      }
      drmModeEncoder* encoder = connector->encoder_id != 0 ? drmModeGetEncoder(fd, connector->encoder_id) : nullptr;
      drmModeFreeConnector(connector);
      if (encoder == nullptr) {
        return nullptr;
      }
      drmModeCrtc* crtc = encoder->crtc_id != 0 ? drmModeGetCrtc(fd, encoder->crtc_id) : nullptr;
      drmModeFreeEncoder(encoder);
      if (crtc == nullptr) {
        return nullptr;
      }
      const uint32_t fbId = crtc->mode_valid != 0 ? crtc->buffer_id : 0;
      drmModeFreeCrtc(crtc);
      drmModeFB2* fb = fbId != 0 ? drmModeGetFB2(fd, fbId) : nullptr;
      if (fb == nullptr) {
        return nullptr;
      }

      auto* held = new HeldBuffer{};
      held->dmabuf.width = static_cast<int32_t>(fb->width);
      held->dmabuf.height = static_cast<int32_t>(fb->height);
      held->dmabuf.format = fb->pixel_format;
      held->dmabuf.modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) != 0 ? fb->modifier : DRM_FORMAT_MOD_INVALID;
      bool ok = fb->handles[0] != 0;
      for (int i = 0; ok && i < 4 && fb->handles[i] != 0; ++i) {
        int planeFd = -1;
        ok = drmPrimeHandleToFD(fd, fb->handles[i], DRM_CLOEXEC, &planeFd) == 0;
        if (ok) {
          held->dmabuf.fd[i] = planeFd;
          held->dmabuf.offset[i] = fb->offsets[i];
          held->dmabuf.stride[i] = fb->pitches[i];
          held->dmabuf.n_planes = i + 1;
        }
      }
      std::array<uint32_t, 4> closed{};
      for (int i = 0; i < 4 && fb->handles[i] != 0; ++i) {
        if (std::ranges::find(closed, fb->handles[i]) == closed.end()) {
          drmCloseBufferHandle(fd, fb->handles[i]);
          closed[static_cast<std::size_t>(i)] = fb->handles[i];
        }
      }
      drmModeFreeFB2(fb);
      if (!ok) {
        wlr_dmabuf_attributes_finish(&held->dmabuf);
        delete held;
        return nullptr;
      }
      wlr_buffer_init(&held->base, &kHeldBufferImpl, held->dmabuf.width, held->dmabuf.height);
      return &held->base;
    }
#else
    wlr_buffer* exportCrtcFrame(wlr_output*) { return nullptr; }
#endif

  } // namespace

  std::unique_ptr<HeldFrame> HeldFrame::capture(wlr_output* output, wlr_scene_tree* parent) {
    wlr_buffer* buffer = exportCrtcFrame(output);
    if (buffer == nullptr) {
      return nullptr;
    }
    wlr_scene_buffer* node = wlr_scene_buffer_create(parent, buffer);
    // The scene node holds its own reference; the frame dies with the node.
    wlr_buffer_drop(buffer);
    if (node == nullptr) {
      return nullptr;
    }
    kLog.info("output '{}': holding the previous frame ({}x{})", output->name, buffer->width, buffer->height);
    return std::unique_ptr<HeldFrame>(new HeldFrame(node));
  }

  HeldFrame::HeldFrame(wlr_scene_buffer* node) : m_node(node) {}

  HeldFrame::~HeldFrame() { wlr_scene_node_destroy(&m_node->node); }

  void HeldFrame::setBox(int x, int y, int width, int height) {
    m_x = x;
    m_y = y;
    m_width = width;
    m_height = height;
    apply();
  }

  void HeldFrame::release(bool reducedMotion) {
    if (m_released) {
      return;
    }
    m_released = true;
    m_reducedMotion = reducedMotion;
    m_progress = desk::motion::Spring(0.0, 1.0, reducedMotion ? kFadeOut : kClose);
    m_progress.setThreshold(1.0 / 256.0);
  }

  bool HeldFrame::step(double seconds) {
    if (!m_released) {
      return true;
    }
    m_progress.step(seconds);
    apply();
    return !m_progress.settled();
  }

  void HeldFrame::apply() {
    if (m_width <= 0 || m_height <= 0) {
      return;
    }
    const double t = m_released ? std::clamp(m_progress.value(), 0.0, 1.0) : 0.0;
    const double scale = 1.0 + (m_reducedMotion ? 0.0 : kExitScale * t);
    const int width = static_cast<int>(std::lround(m_width * scale));
    const int height = static_cast<int>(std::lround(m_height * scale));
    wlr_scene_buffer_set_dest_size(m_node, width, height);
    wlr_scene_node_set_position(&m_node->node, m_x - (width - m_width) / 2, m_y - (height - m_height) / 2);
    wlr_scene_buffer_set_opacity(m_node, static_cast<float>(1.0 - t));
  }

} // namespace andromeda
