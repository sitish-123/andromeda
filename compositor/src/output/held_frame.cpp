#include "output/held_frame.h"

#include "core/log.h"
#include "wlr.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <utility>
#include <vector>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
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
    struct ScanoutPlane {
      uint32_t plane = 0;
      uint32_t fb = 0;
      uint64_t zpos = 0;
      bool coversMode = false;
      uint32_t fbProperty = 0;
      uint32_t crtcProperty = 0;
    };

    // The DRM fd of a DRM output and the id of the CRTC that drives its connector; crtc 0 when there is none.
    std::pair<int, uint32_t> outputCrtc(wlr_output* output, drmModeModeInfo* mode) {
      if (!wlr_output_is_drm(output)) {
        return {-1, 0};
      }
      const int fd = wlr_backend_get_drm_fd(output->backend);
      if (fd < 0) {
        return {-1, 0};
      }
      drmModeConnector* connector = drmModeGetConnector(fd, wlr_drm_connector_get_id(output));
      if (connector == nullptr) {
        return {fd, 0};
      }
      drmModeEncoder* encoder = connector->encoder_id != 0 ? drmModeGetEncoder(fd, connector->encoder_id) : nullptr;
      drmModeFreeConnector(connector);
      if (encoder == nullptr) {
        return {fd, 0};
      }
      drmModeCrtc* crtc = encoder->crtc_id != 0 ? drmModeGetCrtc(fd, encoder->crtc_id) : nullptr;
      drmModeFreeEncoder(encoder);
      if (crtc == nullptr || crtc->mode_valid == 0) {
        drmModeFreeCrtc(crtc);
        return {fd, 0};
      }
      *mode = crtc->mode;
      const uint32_t id = crtc->crtc_id;
      drmModeFreeCrtc(crtc);
      return {fd, id};
    }

    // Every plane that scans out a framebuffer on `crtcId`, with its stacking order and whether it fills the mode.
    std::vector<ScanoutPlane> scanoutPlanes(int fd, uint32_t crtcId, const drmModeModeInfo& mode) {
      std::vector<ScanoutPlane> planes;
      drmModePlaneRes* resources = drmModeGetPlaneResources(fd);
      if (resources == nullptr) {
        return planes;
      }
      for (uint32_t i = 0; i < resources->count_planes; ++i) {
        drmModePlane* plane = drmModeGetPlane(fd, resources->planes[i]);
        if (plane == nullptr) {
          continue;
        }
        if (plane->crtc_id == crtcId && plane->fb_id != 0) {
          ScanoutPlane entry{.plane = plane->plane_id, .fb = plane->fb_id};
          uint64_t x = 1;
          uint64_t y = 1;
          uint64_t w = 0;
          uint64_t h = 0;
          if (drmModeObjectProperties* props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE)) {
            for (uint32_t p = 0; p < props->count_props; ++p) {
              drmModePropertyRes* property = drmModeGetProperty(fd, props->props[p]);
              if (property == nullptr) {
                continue;
              }
              const std::string_view name = property->name;
              const uint64_t value = props->prop_values[p];
              if (name == "zpos") {
                entry.zpos = value;
              } else if (name == "CRTC_X") {
                x = value;
              } else if (name == "CRTC_Y") {
                y = value;
              } else if (name == "CRTC_W") {
                w = value;
              } else if (name == "CRTC_H") {
                h = value;
              } else if (name == "FB_ID") {
                entry.fbProperty = property->prop_id;
              } else if (name == "CRTC_ID") {
                entry.crtcProperty = property->prop_id;
              }
              drmModeFreeProperty(property);
            }
            drmModeFreeObjectProperties(props);
          }
          entry.coversMode = x == 0 && y == 0 && w == mode.hdisplay && h == mode.vdisplay;
          planes.push_back(entry);
        }
        drmModeFreePlane(plane);
      }
      drmModeFreePlaneResources(resources);
      return planes;
    }

    // The buffer behind an fb id, as the inode of its dmabuf export: fb ids are reused, buffers are not. The dmabuf fd
    // stays open in `keep` so the buffer (and its inode) cannot be freed and reused while it is compared.
    ino_t bufferInode(int fd, uint32_t fbId, std::shared_ptr<void>* keep) {
      drmModeFB2* fb = drmModeGetFB2(fd, fbId);
      if (fb == nullptr || fb->handles[0] == 0) {
        drmModeFreeFB2(fb);
        return 0;
      }
      int dmabuf = -1;
      const bool exported = drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC, &dmabuf) == 0;
      std::array<uint32_t, 4> closed{};
      for (int i = 0; i < 4 && fb->handles[i] != 0; ++i) {
        if (std::ranges::find(closed, fb->handles[i]) == closed.end()) {
          drmCloseBufferHandle(fd, fb->handles[i]);
          closed[static_cast<std::size_t>(i)] = fb->handles[i];
        }
      }
      drmModeFreeFB2(fb);
      struct stat info{};
      if (!exported || fstat(dmabuf, &info) != 0) {
        if (dmabuf >= 0) {
          close(dmabuf);
        }
        return 0;
      }
      if (keep != nullptr) {
        *keep = std::shared_ptr<void>(nullptr, [dmabuf](void*) { close(dmabuf); });
      } else {
        close(dmabuf);
      }
      return info.st_ino;
    }

    // GETFB2 on the topmost plane that fills this output's CRTC (with libliftoff the greeter's frame can sit on an
    // overlay above a stale primary), exported as dmabuf fds. GEM handles are closed again: the fds keep the buffer
    // alive after the next commit replaces it.
    wlr_buffer* exportCrtcFrame(wlr_output* output) {
      drmModeModeInfo mode{};
      const auto [fd, crtcId] = outputCrtc(output, &mode);
      if (crtcId == 0) {
        return nullptr;
      }
      uint32_t fbId = 0;
      uint64_t top = 0;
      for (const ScanoutPlane& plane : scanoutPlanes(fd, crtcId, mode)) {
        if (plane.coversMode && (fbId == 0 || plane.zpos >= top)) {
          fbId = plane.fb;
          top = plane.zpos;
        }
      }
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

#if ANDROMEDA_HAS_NATIVE_DRM_POLICY
  std::vector<ForeignPlane> foreignPlanes(wlr_output* output) {
    drmModeModeInfo mode{};
    const auto [fd, crtcId] = outputCrtc(output, &mode);
    std::vector<ForeignPlane> planes;
    if (crtcId == 0) {
      return planes;
    }
    for (const ScanoutPlane& plane : scanoutPlanes(fd, crtcId, mode)) {
      ForeignPlane entry{.plane = plane.plane, .crtc = crtcId, .fb = plane.fb, .buffer = 0, .keep = nullptr};
      entry.buffer = bufferInode(fd, plane.fb, &entry.keep);
      if (entry.buffer != 0) {
        planes.push_back(std::move(entry));
      }
    }
    return planes;
  }

  int disableForeignPlanes(wlr_output* output, const std::vector<ForeignPlane>& before) {
    drmModeModeInfo mode{};
    const auto [fd, crtcId] = outputCrtc(output, &mode);
    if (crtcId == 0 || before.empty() || before.front().crtc != crtcId) {
      return 0;
    }
    drmModeAtomicReq* request = drmModeAtomicAlloc();
    if (request == nullptr) {
      return 0;
    }
    int count = 0;
    for (const ScanoutPlane& plane : scanoutPlanes(fd, crtcId, mode)) {
      const bool stale = std::ranges::any_of(before, [&](const ForeignPlane& old) {
        return old.plane == plane.plane && old.fb == plane.fb && bufferInode(fd, plane.fb, nullptr) == old.buffer;
      });
      if (stale && plane.fbProperty != 0 && plane.crtcProperty != 0) {
        drmModeAtomicAddProperty(request, plane.plane, plane.fbProperty, 0);
        drmModeAtomicAddProperty(request, plane.plane, plane.crtcProperty, 0);
        ++count;
      }
    }
    // Blocking: it runs in the present handler, after the page flip completed, so no flip is pending; the output misses
    // one frame once. Without atomic modesetting (WLR_DRM_NO_ATOMIC) the commit fails with EINVAL and is skipped.
    if (count > 0 && drmModeAtomicCommit(fd, request, 0, nullptr) != 0) {
      if (errno != EINVAL && errno != EOPNOTSUPP) {
        kLog.warn("output '{}': could not turn off {} planes left by the previous master: {}", output->name, count,
                  std::strerror(errno));
      }
      count = 0;
    }
    drmModeAtomicFree(request);
    return count;
  }
#else
  std::vector<ForeignPlane> foreignPlanes(wlr_output*) { return {}; }
  int disableForeignPlanes(wlr_output*, const std::vector<ForeignPlane>&) { return 0; }
#endif

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
    return std::unique_ptr<HeldFrame>(new HeldFrame(node, buffer->width, buffer->height));
  }

  bool HeldFrame::loginHandoff() {
    const char* value = std::getenv("ANDROMEDA_LOGIN_HANDOFF");
    if (value == nullptr) {
      return false;
    }
    char* end = nullptr;
    const long long stamp = std::strtoll(value, &end, 10);
    const long long now = static_cast<long long>(std::time(nullptr));
    return end != value && *end == '\0' && stamp <= now && now - stamp <= 15;
  }

  HeldFrame::HeldFrame(wlr_scene_buffer* node, int bufferWidth, int bufferHeight)
      : m_node(node), m_bufferWidth(bufferWidth), m_bufferHeight(bufferHeight) {}

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
