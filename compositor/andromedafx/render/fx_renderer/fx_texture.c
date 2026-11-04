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
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wayland-server-core.h>
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

// Below this size a damage-limited copy costs less than a per-commit udmabuf import.
#define SHM_IMPORT_MIN_BYTES (1 << 20)

// A dmabuf view of the page-aligned range of a client shm buffer. It keeps the
// client buffer locked so the client does not write it while it is sampled.
struct shm_import {
  struct wlr_buffer base;
  struct wlr_buffer* source;
  struct wl_event_loop* loop;
  struct wlr_dmabuf_attributes dmabuf;
};

struct shm_release {
  struct wlr_buffer* source;
  struct wl_event_source* event;
  int fence;
};

static int shm_release_handle(int fd, uint32_t mask, void* data) {
  struct shm_release* release = data;
  wl_event_source_remove(release->event);
  close(release->fence);
  wlr_buffer_unlock(release->source);
  free(release);
  return 0;
}

// The client may write the buffer once it is released, so wait for the GPU
// reads fenced on the dmabuf before the unlock.
static void shm_import_release_source(struct shm_import* import) {
  struct dma_buf_export_sync_file req = {.flags = DMA_BUF_SYNC_WRITE, .fd = -1};
  if (ioctl(import->dmabuf.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) == 0) {
    struct pollfd pfd = {.fd = req.fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) != 1) {
      struct shm_release* release = calloc(1, sizeof(*release));
      if (release != NULL) {
        release->source = import->source;
        release->fence = req.fd;
        release->event = wl_event_loop_add_fd(import->loop, req.fd, WL_EVENT_READABLE, shm_release_handle, release);
        if (release->event != NULL) {
          return;
        }
        free(release);
      }
    }
    close(req.fd);
  }
  wlr_buffer_unlock(import->source);
}

static void shm_import_destroy(struct wlr_buffer* buffer) {
  struct shm_import* import = wl_container_of(buffer, import, base);
  wlr_buffer_finish(buffer);
  shm_import_release_source(import);
  close(import->dmabuf.fd[0]);
  free(import);
}

static bool shm_import_get_dmabuf(struct wlr_buffer* buffer, struct wlr_dmabuf_attributes* attribs) {
  struct shm_import* import = wl_container_of(buffer, import, base);
  *attribs = import->dmabuf;
  return true;
}

static const struct wlr_buffer_impl shm_import_impl = {
    .destroy = shm_import_destroy,
    .get_dmabuf = shm_import_get_dmabuf,
};

// Samples a large shm buffer in place instead of copying it into a texture.
// Returns NULL when the buffer must take the copy path.
static struct wlr_texture* fx_texture_from_shm_import(struct fx_renderer* renderer, struct wlr_buffer* buffer) {
  // -2: not opened yet, -1: unavailable for this process.
  static int udmabuf_dev = -2;
  // The last stride the driver refused (pitch alignment differs per GPU); 0 = none.
  static int refused_stride = 0;
  struct wlr_shm_attributes shm;
  // Without implicit fences the release could not wait for the GPU reads.
  if (renderer->event_loop == NULL || renderer->egl->ignores_implicit_fences || udmabuf_dev == -1
      || !wlr_buffer_get_shm(buffer, &shm) || shm.stride == refused_stride
      || (shm.format != DRM_FORMAT_ARGB8888 && shm.format != DRM_FORMAT_XRGB8888)
      || (size_t)shm.stride * shm.height < SHM_IMPORT_MIN_BYTES) {
    return NULL;
  }
  if (udmabuf_dev == -2) {
    udmabuf_dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf_dev < 0) {
      wlr_log(WLR_INFO, "Copying shm buffers: /dev/udmabuf: %s", strerror(errno));
      udmabuf_dev = -1;
      return NULL;
    }
  }

  const off_t page = sysconf(_SC_PAGESIZE);
  const off_t start = shm.offset & ~(page - 1);
  const off_t end = (shm.offset + (off_t)shm.stride * shm.height + page - 1) & ~(page - 1);
  struct udmabuf_create create = {
      .memfd = shm.fd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .offset = start,
      .size = end - start,
  };
  // Fails for an unsealed memfd or a range past the end of the file.
  const int fd = ioctl(udmabuf_dev, UDMABUF_CREATE, &create);
  if (fd < 0) {
    return NULL;
  }
  struct shm_import* import = calloc(1, sizeof(*import));
  if (import == NULL) {
    close(fd);
    return NULL;
  }
  wlr_buffer_init(&import->base, &shm_import_impl, shm.width, shm.height);
  import->loop = renderer->event_loop;
  import->source = wlr_buffer_lock(buffer);
  import->dmabuf = (struct wlr_dmabuf_attributes){
      .width = shm.width,
      .height = shm.height,
      .format = shm.format,
      .modifier = DRM_FORMAT_MOD_LINEAR,
      .n_planes = 1,
      .offset = {(uint32_t)(shm.offset - start)},
      .stride = {(uint32_t)shm.stride},
      .fd = {fd, -1, -1, -1},
  };

  struct wlr_texture* texture = fx_texture_from_dmabuf(&renderer->wlr_renderer, &import->base);
  if (texture == NULL) {
    wlr_log(WLR_DEBUG, "Copying shm buffers with stride %d: EGL refused a linear %dx%d import", shm.stride, shm.width,
            shm.height);
    refused_stride = shm.stride;
  }
  wlr_buffer_drop(&import->base);
  return texture;
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
  } else if ((imported = fx_texture_from_shm_import(renderer, buffer)) != NULL) {
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
