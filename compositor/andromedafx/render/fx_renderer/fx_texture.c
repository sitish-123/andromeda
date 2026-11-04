#define _GNU_SOURCE
#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/pass.h"
#include "render/pixel_format.h"
#include "andromedafx/render/fx_renderer/fx_offscreen_buffers.h"
#include "andromedafx/render/fx_renderer/fx_renderer.h"

#include <assert.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

static const struct wlr_texture_impl texture_impl;

bool wlr_texture_is_fx(struct wlr_texture* wlr_texture) { return wlr_texture->impl == &texture_impl; }

struct fx_texture* fx_get_texture(struct wlr_texture* wlr_texture) {
  assert(wlr_texture_is_fx(wlr_texture));
  struct fx_texture* texture = wl_container_of(wlr_texture, texture, wlr_texture);
  return texture;
}

// Below this size the plain GL upload costs less than a staging buffer.
#define SHM_STAGING_MIN_BYTES (1 << 20)

// A compositor-owned linear buffer the GPU samples in place: a udmabuf over a
// sealed memfd. The CPU copies client damage into it, so the client buffer is
// released at once and no GPU upload blit runs.
struct shm_staging {
  struct wlr_buffer base;
  struct wlr_dmabuf_attributes dmabuf;
  uint8_t* map;
  size_t size;
};

static void shm_staging_destroy(struct wlr_buffer* buffer) {
  struct shm_staging* staging = wl_container_of(buffer, staging, base);
  wlr_buffer_finish(buffer);
  munmap(staging->map, staging->size);
  close(staging->dmabuf.fd[0]);
  free(staging);
}

static bool shm_staging_get_dmabuf(struct wlr_buffer* buffer, struct wlr_dmabuf_attributes* attribs) {
  struct shm_staging* staging = wl_container_of(buffer, staging, base);
  *attribs = staging->dmabuf;
  return true;
}

static const struct wlr_buffer_impl shm_staging_impl = {
    .destroy = shm_staging_destroy,
    .get_dmabuf = shm_staging_get_dmabuf,
};

// Opens /dev/udmabuf for one slot; -1 when the session has no access (logged once).
static int udmabuf_open(void) {
  static bool logged = false;
  const int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
  if (dev < 0 && !logged) {
    wlr_log(WLR_INFO, "Uploading shm buffers through GL: /dev/udmabuf: %s", strerror(errno));
    logged = true;
  }
  return dev;
}

// Allocates one staging slot and its GL texture; needs the EGL context current.
static struct wlr_buffer* shm_slot_create(struct fx_renderer* renderer, uint32_t format, int width, int height,
                                          GLuint* tex) {
  const int dev = udmabuf_open();
  if (dev < 0) {
    return NULL;
  }
  // 256-byte pitch alignment satisfies the linear sampling rules of Intel and AMD.
  const uint32_t stride = ((uint32_t)width * 4 + 255) & ~255u;
  const size_t page = sysconf(_SC_PAGESIZE);
  const size_t size = ((size_t)stride * height + page - 1) & ~(page - 1);
  const int memfd = memfd_create("andromeda-shm-staging", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (memfd < 0) {
    close(dev);
    return NULL;
  }
  int fd = -1;
  void* map = MAP_FAILED;
  if (ftruncate(memfd, size) == 0 && fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  }
  if (map != MAP_FAILED) {
    struct udmabuf_create create = {.memfd = memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size};
    fd = ioctl(dev, UDMABUF_CREATE, &create);
  }
  close(dev);
  close(memfd);
  struct shm_staging* staging = fd >= 0 ? calloc(1, sizeof(*staging)) : NULL;
  if (staging == NULL) {
    if (fd >= 0) {
      close(fd);
    }
    if (map != MAP_FAILED) {
      munmap(map, size);
    }
    return NULL;
  }
  wlr_buffer_init(&staging->base, &shm_staging_impl, width, height);
  staging->map = map;
  staging->size = size;
  staging->dmabuf = (struct wlr_dmabuf_attributes){
      .width = width,
      .height = height,
      .format = format,
      .modifier = DRM_FORMAT_MOD_LINEAR,
      .n_planes = 1,
      .offset = {0},
      .stride = {stride},
      .fd = {fd, -1, -1, -1},
  };
  struct fx_framebuffer* framebuffer = fx_framebuffer_get_or_create(renderer, &staging->base);
  *tex = framebuffer != NULL && !framebuffer->external_only ? fx_framebuffer_get_texture(framebuffer) : 0;
  if (*tex == 0) {
    wlr_buffer_drop(&staging->base);
    return NULL;
  }
  return &staging->base;
}

// Copies `region` of the client pixels into a staging slot. The sync ioctl
// waits until the GPU no longer reads the slot.
static void shm_slot_write(struct wlr_buffer* slot, const pixman_region32_t* region, const uint8_t* data,
                           size_t src_stride) {
  struct shm_staging* staging = wl_container_of(slot, staging, base);
  const size_t dst_stride = staging->dmabuf.stride[0];
  struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
  int ret;
  do {
    ret = ioctl(staging->dmabuf.fd[0], DMA_BUF_IOCTL_SYNC, &sync);
  } while (ret != 0 && (errno == EINTR || errno == EAGAIN));
  int rects_len = 0;
  const pixman_box32_t* rects = pixman_region32_rectangles(region, &rects_len);
  for (int i = 0; i < rects_len; i++) {
    const size_t x = (size_t)rects[i].x1 * 4;
    const size_t len = (size_t)(rects[i].x2 - rects[i].x1) * 4;
    for (int y = rects[i].y1; y < rects[i].y2; y++) {
      memcpy(staging->map + y * dst_stride + x, data + y * src_stride + x, len);
    }
  }
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
  ioctl(staging->dmabuf.fd[0], DMA_BUF_IOCTL_SYNC, &sync);
}

static bool fx_texture_update_from_buffer(
    struct wlr_texture* wlr_texture, struct wlr_buffer* buffer, const pixman_region32_t* damage
) {
  struct fx_texture* texture = fx_get_texture(wlr_texture);

  if (texture->drm_format == DRM_FORMAT_INVALID) {
    return false;
  }

  void* data;
  uint32_t format;
  size_t stride;
  if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
    return false;
  }

  if (format != texture->drm_format) {
    wlr_buffer_end_data_ptr_access(buffer);
    return false;
  }

  const struct fx_pixel_format* fmt = get_fx_format_from_drm(texture->drm_format);
  assert(fmt);

  const struct wlr_pixel_format_info* drm_fmt = drm_get_pixel_format_info(texture->drm_format);
  assert(drm_fmt);
  if (pixel_format_info_pixels_per_block(drm_fmt) != 1) {
    wlr_buffer_end_data_ptr_access(buffer);
    wlr_log(WLR_ERROR, "Cannot update texture: block formats are not supported");
    return false;
  }

  if (!pixel_format_info_check_stride(drm_fmt, stride, buffer->width)) {
    wlr_buffer_end_data_ptr_access(buffer);
    return false;
  }

  if (texture->slots[0] != NULL) {
    pixman_region32_t region;
    pixman_region32_init(&region);
    pixman_region32_copy(&region, damage);
    const int next = texture->slot ^ 1;
    if (texture->slots[next] == NULL && !texture->slot_failed) {
      struct wlr_egl_context prev_ctx;
      wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx);
      texture->slots[next] = shm_slot_create(
          texture->fx_renderer, texture->drm_format, buffer->width, buffer->height, &texture->slot_tex[next]
      );
      wlr_egl_restore_context(&prev_ctx);
      texture->slot_failed = texture->slots[next] == NULL;
      pixman_region32_union_rect(&region, &region, 0, 0, buffer->width, buffer->height);
    } else {
      pixman_region32_union(&region, &region, &texture->slot_stale);
    }
    // Alternating slots keeps the CPU off the slot the last frame sampled.
    if (texture->slots[next] != NULL) {
      pixman_region32_copy(&texture->slot_stale, damage);
      texture->slot = next;
      texture->tex = texture->slot_tex[next];
      // read_pixels attached the old slot's texture to this framebuffer.
      if (texture->fbo != 0) {
        struct wlr_egl_context prev_ctx;
        wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx);
        glDeleteFramebuffers(1, &texture->fbo);
        wlr_egl_restore_context(&prev_ctx);
        texture->fbo = 0;
      }
    } else {
      pixman_region32_copy(&region, damage);
    }
    shm_slot_write(texture->slots[texture->slot], &region, data, stride);
    pixman_region32_fini(&region);
    wlr_buffer_end_data_ptr_access(buffer);
    return true;
  }

  struct wlr_egl_context prev_ctx;
  wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx);

  push_fx_debug(texture->fx_renderer);

  glBindTexture(GL_TEXTURE_2D, texture->tex);

  int rects_len = 0;
  pixman_box32_t* rects = pixman_region32_rectangles(damage, &rects_len);

  for (int i = 0; i < rects_len; i++) {
    pixman_box32_t rect = rects[i];

    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / drm_fmt->bytes_per_block);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rect.x1);
    glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rect.y1);

    int width = rect.x2 - rect.x1;
    int height = rect.y2 - rect.y1;
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, rect.x1, rect.y1, width, height, fmt->gl_format,
        fx_resolve_gl_type(texture->fx_renderer, fmt->gl_type), data
    );
  }

  glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);

  glBindTexture(GL_TEXTURE_2D, 0);

  pop_fx_debug(texture->fx_renderer);

  wlr_egl_restore_context(&prev_ctx);

  wlr_buffer_end_data_ptr_access(buffer);

  return true;
}

void fx_texture_destroy(struct fx_texture* texture) {
  wl_list_remove(&texture->link);
  if (texture->buffer != NULL) {
    wlr_buffer_unlock(texture->locked_buffer);
  } else if (texture->slots[0] != NULL) {
    struct wlr_egl_context prev_ctx;
    wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx);
    glDeleteFramebuffers(1, &texture->fbo);
    wlr_egl_restore_context(&prev_ctx);
    // The slot framebuffers own the GL textures.
    wlr_buffer_drop(texture->slots[0]);
    if (texture->slots[1] != NULL) {
      wlr_buffer_drop(texture->slots[1]);
    }
    pixman_region32_fini(&texture->slot_stale);
  } else {
    struct wlr_egl_context prev_ctx;
    wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx);

    push_fx_debug(texture->fx_renderer);

    glDeleteTextures(1, &texture->tex);
    glDeleteFramebuffers(1, &texture->fbo);

    pop_fx_debug(texture->fx_renderer);

    wlr_egl_restore_context(&prev_ctx);
  }

  free(texture);
}

static bool fx_texture_bind(struct fx_texture* texture) {
  if (texture->fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, texture->fbo);
  } else if (texture->buffer) {
    if (texture->buffer->external_only) {
      return false;
    }

    GLuint fbo = fx_framebuffer_get_fbo(texture->buffer);
    if (!fbo) {
      return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  } else {
    glGenFramebuffers(1, &texture->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, texture->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture->target, texture->tex, 0);

    GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
      wlr_log(WLR_ERROR, "Failed to create FBO");
      glDeleteFramebuffers(1, &texture->fbo);
      texture->fbo = 0;

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      return false;
    }
  }

  return true;
}

static bool
fx_texture_read_pixels(struct wlr_texture* wlr_texture, const struct wlr_texture_read_pixels_options* options) {
  struct fx_texture* texture = fx_get_texture(wlr_texture);

  struct wlr_box src;
  wlr_texture_read_pixels_options_get_src_box(options, wlr_texture, &src);

  const struct fx_pixel_format* fmt = get_fx_format_from_drm(options->format);
  if (fmt == NULL) {
    wlr_log(WLR_ERROR, "Cannot read pixels: unsupported pixel format 0x%" PRIX32, options->format);
    return false;
  }

  if (fmt->gl_format == GL_BGRA_EXT && !texture->fx_renderer->exts.EXT_read_format_bgra) {
    wlr_log(WLR_ERROR, "Cannot read pixels: missing GL_EXT_read_format_bgra extension");
    return false;
  }

  const struct wlr_pixel_format_info* drm_fmt = drm_get_pixel_format_info(fmt->drm_format);
  assert(drm_fmt);
  if (pixel_format_info_pixels_per_block(drm_fmt) != 1) {
    wlr_log(WLR_ERROR, "Cannot read pixels: block formats are not supported");
    return false;
  }

  struct wlr_egl_context prev_ctx;
  if (!wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx)) {
    return false;
  }
  push_fx_debug(texture->fx_renderer);

  bool ok = false;
  if (!fx_texture_bind(texture)) {
    goto out;
  }

  const GLenum read_gl_type = fx_resolve_gl_type(texture->fx_renderer, fmt->gl_type);

  if (!is_fx_pixel_format_supported(texture->fx_renderer, fmt)) {
    GLint read_format = 0, read_type = 0;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &read_format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &read_type);
    // Readback can support a format without its texture-upload extension.
    if (read_format != fmt->gl_format || (GLenum)read_type != read_gl_type) {
      wlr_log(WLR_ERROR, "Cannot read pixels: unsupported pixel format 0x%" PRIX32, options->format);
      goto out;
    }
  }

  // Make sure any pending drawing is finished before we try to read it
  glFinish();

  glGetError(); // Clear the error flag

  unsigned char* p = wlr_texture_read_pixel_options_get_data(options);

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  uint32_t pack_stride = pixel_format_info_min_stride(drm_fmt, src.width);
  if (pack_stride == options->stride && options->dst_x == 0) {
    // Under these particular conditions, we can read the pixels with only
    // one glReadPixels call

    glReadPixels(src.x, src.y, src.width, src.height, fmt->gl_format, read_gl_type, p);
  } else {
    // Unfortunately GLES2 doesn't support GL_PACK_ROW_LENGTH, so we have to read
    // the lines out row by row
    for (int32_t i = 0; i < src.height; ++i) {
      uint32_t y = src.y + i;
      glReadPixels(src.x, y, src.width, 1, fmt->gl_format, read_gl_type, p + i * options->stride);
    }
  }

  ok = glGetError() == GL_NO_ERROR;
out:
  pop_fx_debug(texture->fx_renderer);
  wlr_egl_restore_context(&prev_ctx);
  return ok;
}

static uint32_t fx_texture_preferred_read_format(struct wlr_texture* wlr_texture) {
  struct fx_texture* texture = fx_get_texture(wlr_texture);

  push_fx_debug(texture->fx_renderer);

  uint32_t fmt = DRM_FORMAT_INVALID;

  struct wlr_egl_context prev_ctx;
  if (!wlr_egl_make_current(texture->fx_renderer->egl, &prev_ctx)) {
    return fmt;
  }

  if (!fx_texture_bind(texture)) {
    goto out;
  }

  GLint gl_format = -1, gl_type = -1, alpha_size = -1;
  glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &gl_format);
  glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &gl_type);
  glGetIntegerv(GL_ALPHA_BITS, &alpha_size);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  pop_fx_debug(texture->fx_renderer);

  // GLES2 can always read a framebuffer back as GL_RGBA/GL_UNSIGNED_BYTE, and
  // as GL_BGRA_EXT with EXT_read_format_bgra. Capture clients are commonly
  // written against the BGRA byte order, but drivers report GL_RGBA for 8-bit
  // targets without a real alpha channel (Mesa does for XRGB8888), so 8-bit
  // readback publishes the BGRA order whenever the driver can read it.
  // Packed 24-bit RGB (the NVIDIA proprietary driver reports GL_RGB for opaque
  // targets) has rows that are not 4-byte aligned, which clients assuming a
  // 4-byte pixel turn into a stride wl_shm rejects, so it is read as 32-bit.
  bool bgra = texture->fx_renderer->exts.EXT_read_format_bgra;
  // Shared-memory capture is the portable SDR path. Keep packed 10-bit
  // framebuffers intact for rendering and DMA-BUF, but publish an 8-bit format
  // that glReadPixels can produce through normalized depth conversion.
  if (texture->buffer != NULL) {
    switch (texture->buffer->drm_format) {
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XBGR2101010:
      fmt = bgra ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XBGR8888;
      goto out;
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
      fmt = bgra ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_ABGR8888;
      goto out;
    }
  }
  const struct fx_pixel_format* pix_fmt = get_fx_format_from_gl(gl_format, gl_type, alpha_size > 0);
  switch (pix_fmt != NULL ? pix_fmt->drm_format : DRM_FORMAT_INVALID) {
  case DRM_FORMAT_ABGR8888:
    fmt = bgra ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_ABGR8888;
    break;
  case DRM_FORMAT_XBGR8888:
  case DRM_FORMAT_BGR888:
  case DRM_FORMAT_INVALID:
    fmt = bgra ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XBGR8888;
    break;
  default:
    fmt = pix_fmt->drm_format;
    break;
  }

out:
  wlr_egl_restore_context(&prev_ctx);
  return fmt;
}

static void texture_handle_buffer_destroy(struct wlr_texture* wlr_texture) {
  fx_texture_destroy(fx_get_texture(wlr_texture));
}

static const struct wlr_texture_impl texture_impl = {
    .update_from_buffer = fx_texture_update_from_buffer,
    .read_pixels = fx_texture_read_pixels,
    .preferred_read_format = fx_texture_preferred_read_format,
    .destroy = texture_handle_buffer_destroy,
};

static struct fx_texture* fx_texture_create(struct fx_renderer* renderer, uint32_t width, uint32_t height) {
  struct fx_texture* texture = calloc(1, sizeof(struct fx_texture));
  if (texture == NULL) {
    wlr_log_errno(WLR_ERROR, "Allocation failed");
    return NULL;
  }
  wlr_texture_init(&texture->wlr_texture, &renderer->wlr_renderer, &texture_impl, width, height);
  texture->fx_renderer = renderer;
  wl_list_insert(&renderer->textures, &texture->link);
  return texture;
}

static struct wlr_texture* fx_texture_from_pixels(
    struct wlr_renderer* wlr_renderer, uint32_t drm_format, uint32_t stride, uint32_t width, uint32_t height,
    const void* data
) {
  struct fx_renderer* renderer = fx_get_renderer(wlr_renderer);

  const struct fx_pixel_format* fmt = get_fx_format_from_drm(drm_format);
  if (fmt == NULL) {
    wlr_log(WLR_ERROR, "Unsupported pixel format 0x%" PRIX32, drm_format);
    return NULL;
  }

  const struct wlr_pixel_format_info* drm_fmt = drm_get_pixel_format_info(drm_format);
  assert(drm_fmt);
  if (pixel_format_info_pixels_per_block(drm_fmt) != 1) {
    wlr_log(WLR_ERROR, "Cannot upload texture: block formats are not supported");
    return NULL;
  }

  if (!pixel_format_info_check_stride(drm_fmt, stride, width)) {
    return NULL;
  }

  struct fx_texture* texture = fx_texture_create(renderer, width, height);
  if (texture == NULL) {
    return NULL;
  }
  texture->target = GL_TEXTURE_2D;
  texture->has_alpha = pixel_format_has_alpha(fmt->drm_format);
  texture->drm_format = fmt->drm_format;

  GLint internal_format = fx_resolve_internal_format(renderer, fmt);

  struct wlr_egl_context prev_ctx;
  wlr_egl_make_current(renderer->egl, &prev_ctx);

  push_fx_debug(renderer);

  glGenTextures(1, &texture->tex);
  glBindTexture(GL_TEXTURE_2D, texture->tex);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / drm_fmt->bytes_per_block);
  glTexImage2D(
      GL_TEXTURE_2D, 0, internal_format, width, height, 0, fmt->gl_format, fx_resolve_gl_type(renderer, fmt->gl_type),
      data
  );
  glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

  glBindTexture(GL_TEXTURE_2D, 0);

  pop_fx_debug(renderer);

  wlr_egl_restore_context(&prev_ctx);

  return &texture->wlr_texture;
}

static struct wlr_texture* fx_texture_from_framebuffer(
    struct fx_renderer* renderer, struct fx_framebuffer* buffer, struct wlr_buffer* locked_buffer
) {
  if (!renderer->procs.glEGLImageTargetTexture2DOES) {
    return NULL;
  }

  struct fx_texture* texture = fx_texture_create(renderer, buffer->buffer->width, buffer->buffer->height);
  if (texture == NULL) {
    return NULL;
  }
  texture->target = buffer->external_only ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
  texture->buffer = buffer;
  texture->drm_format = DRM_FORMAT_INVALID; // texture can't be written anyways
  texture->has_alpha = pixel_format_has_alpha(buffer->drm_format);

  struct wlr_egl_context prev_ctx;
  wlr_egl_make_current(renderer->egl, &prev_ctx);
  push_fx_debug(texture->fx_renderer);

  const GLuint tex = fx_framebuffer_get_texture(buffer);

  pop_fx_debug(texture->fx_renderer);
  wlr_egl_restore_context(&prev_ctx);

  if (tex == 0) {
    wl_list_remove(&texture->link);
    free(texture);
    return NULL;
  }
  texture->tex = tex;
  texture->locked_buffer = wlr_buffer_lock(locked_buffer);
  return &texture->wlr_texture;
}

bool fx_framebuffer_copy(
    struct fx_framebuffer* target, struct fx_framebuffer* source, enum wlr_color_transfer_function transfer_function,
    enum wlr_color_transfer_function target_tf
) {
  struct fx_renderer* renderer = target->renderer;
  struct wlr_texture* texture = fx_texture_from_framebuffer(renderer, source, source->buffer);
  if (texture == NULL) {
    return false;
  }

  struct wlr_render_pass* pass = wlr_renderer_begin_buffer_pass(&renderer->wlr_renderer, target->buffer, NULL);
  if (pass == NULL) {
    wlr_texture_destroy(texture);
    return false;
  }
  // The target is ours and only this context reads it.
  fx_get_render_pass(pass)->implicit_sync_target = false;
  fx_get_render_pass(pass)->sdr_tf = target_tf;

  struct wlr_box box = {
      .width = target->buffer->width,
      .height = target->buffer->height,
  };
  wlr_render_pass_add_texture(
      pass,
      &(struct wlr_render_texture_options){
          .texture = texture,
          .dst_box = box,
          .filter_mode = WLR_SCALE_FILTER_NEAREST,
          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
          .transfer_function = transfer_function,
      }
  );
  bool ok = wlr_render_pass_submit(pass);
  wlr_texture_destroy(texture);
  return ok;
}

static bool update_sdr_capture_buffer(struct fx_framebuffer* output_buffer) {
  struct fx_offscreen_buffers* output_buffers = output_buffer->output_buffers;
  struct fx_framebuffer* blend_buffer = output_buffer->blend_buffer;
  struct fx_framebuffer** capture_buffer = &output_buffer->sdr_capture_buffer;
  if (output_buffers != NULL) {
    if (output_buffer->output_generation != 0
        && output_buffers->sdr_capture_generation == output_buffer->output_generation
        && output_buffers->sdr_capture_buffer != NULL
        && output_buffers->sdr_capture_buffer->buffer->width == output_buffer->buffer->width
        && output_buffers->sdr_capture_buffer->buffer->height == output_buffer->buffer->height
        && output_buffers->sdr_capture_buffer->drm_format == output_buffer->drm_format) {
      return true;
    }
    if (!output_buffers->blend_valid || output_buffer->output_generation != output_buffers->blend_generation) {
      return false;
    }
    blend_buffer = output_buffers->blend_buffer;
    capture_buffer = &output_buffers->sdr_capture_buffer;
  } else if (output_buffer->sdr_capture_valid) {
    return true;
  }
  if (blend_buffer == NULL) {
    return false;
  }
  if (*capture_buffer != NULL && (*capture_buffer)->buffer->n_locks > 0) {
    wlr_log(WLR_DEBUG, "SDR capture buffer is still in use");
    return false;
  }

  struct fx_renderer* renderer = output_buffer->renderer;
  uint32_t format = output_buffer->drm_format;
  struct wlr_allocator* allocator = output_buffers != NULL ? output_buffers->allocator : renderer->allocator;
  if (allocator == NULL) {
    return false;
  }

  bool failed = false;
  struct wlr_egl_context prev_ctx;
  if (!wlr_egl_make_current(renderer->egl, &prev_ctx)) {
    return false;
  }
  fx_framebuffer_get_or_create_custom(
      renderer, allocator, output_buffer->buffer->width, output_buffer->buffer->height, format, capture_buffer, &failed
  );
  wlr_egl_restore_context(&prev_ctx);
  if (failed || *capture_buffer == NULL) {
    return false;
  }
  if (output_buffers != NULL) {
    output_buffers->sdr_capture_generation = 0;
  } else {
    (*capture_buffer)->sdr_capture_parent = output_buffer;
  }

  bool valid = fx_framebuffer_copy(
      *capture_buffer, blend_buffer, WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, output_buffer->sdr_capture_tf
  );
  if (output_buffers != NULL) {
    output_buffers->sdr_capture_generation = valid ? output_buffer->output_generation : 0;
  } else {
    output_buffer->sdr_capture_valid = valid;
  }
  return valid;
}

static struct wlr_texture* fx_texture_from_dmabuf(struct wlr_renderer* wlr_renderer, struct wlr_buffer* wlr_buffer) {
  struct fx_renderer* renderer = fx_get_renderer(wlr_renderer);
  struct fx_framebuffer* buffer = fx_framebuffer_get_or_create(renderer, wlr_buffer);
  if (buffer == NULL) {
    return NULL;
  }

  struct fx_framebuffer* texture_buffer = buffer;
  if (buffer->effect_capture_valid && buffer->effect_capture_buffer != NULL) {
    texture_buffer = buffer->effect_capture_buffer;
  } else if (buffer->capture_sdr) {
    if (!update_sdr_capture_buffer(buffer)) {
      return NULL;
    }
    texture_buffer =
        buffer->output_buffers != NULL ? buffer->output_buffers->sdr_capture_buffer : buffer->sdr_capture_buffer;
  }
  return fx_texture_from_framebuffer(renderer, texture_buffer, texture_buffer->buffer);
}

// Copies a large shm buffer into staging slots (see struct fx_texture).
// Returns NULL when the buffer must take the plain upload path.
static struct wlr_texture* fx_texture_from_shm_slots(struct fx_renderer* renderer, struct wlr_buffer* buffer) {
  // Without implicit fences the sync ioctl cannot wait for the GPU reads.
  if (renderer->egl->ignores_implicit_fences) {
    return NULL;
  }
  void* data;
  uint32_t format;
  size_t stride;
  if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
    return NULL;
  }
  const struct wlr_pixel_format_info* drm_fmt = drm_get_pixel_format_info(format);
  if ((format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888) || drm_fmt == NULL
      || stride * buffer->height < SHM_STAGING_MIN_BYTES
      || !pixel_format_info_check_stride(drm_fmt, stride, buffer->width)) {
    wlr_buffer_end_data_ptr_access(buffer);
    return NULL;
  }

  struct fx_texture* texture = fx_texture_create(renderer, buffer->width, buffer->height);
  if (texture == NULL) {
    wlr_buffer_end_data_ptr_access(buffer);
    return NULL;
  }
  struct wlr_egl_context prev_ctx;
  wlr_egl_make_current(renderer->egl, &prev_ctx);
  texture->slots[0] = shm_slot_create(renderer, format, buffer->width, buffer->height, &texture->slot_tex[0]);
  wlr_egl_restore_context(&prev_ctx);
  if (texture->slots[0] == NULL) {
    wlr_buffer_end_data_ptr_access(buffer);
    wl_list_remove(&texture->link);
    free(texture);
    return NULL;
  }
  texture->target = GL_TEXTURE_2D;
  texture->has_alpha = pixel_format_has_alpha(format);
  texture->drm_format = format;
  texture->tex = texture->slot_tex[0];
  pixman_region32_init(&texture->slot_stale);
  pixman_region32_t all;
  pixman_region32_init_rect(&all, 0, 0, buffer->width, buffer->height);
  shm_slot_write(texture->slots[0], &all, data, stride);
  pixman_region32_fini(&all);
  wlr_buffer_end_data_ptr_access(buffer);
  return &texture->wlr_texture;
}

struct wlr_texture* fx_texture_from_buffer(struct wlr_renderer* wlr_renderer, struct wlr_buffer* buffer) {
  struct fx_renderer* renderer = fx_get_renderer(wlr_renderer);

  void* data;
  uint32_t format;
  size_t stride;
  struct wlr_dmabuf_attributes dmabuf;
  struct wlr_texture* imported;
  if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
    return fx_texture_from_dmabuf(&renderer->wlr_renderer, buffer);
  } else if ((imported = fx_texture_from_shm_slots(renderer, buffer)) != NULL) {
    return imported;
  } else if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
    struct wlr_texture* tex = fx_texture_from_pixels(wlr_renderer, format, stride, buffer->width, buffer->height, data);
    wlr_buffer_end_data_ptr_access(buffer);
    return tex;
  } else {
    return NULL;
  }
}

void fx_texture_get_attribs(struct wlr_texture* wlr_texture, struct fx_texture_attribs* attribs) {
  struct fx_texture* texture = fx_get_texture(wlr_texture);
  *attribs = (struct fx_texture_attribs){
      .target = texture->target,
      .tex = texture->tex,
      .has_alpha = texture->has_alpha,
  };
}
