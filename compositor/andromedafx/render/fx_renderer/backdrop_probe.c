#include "render/fx_renderer/backdrop_probe.h"
#include "render/egl.h"
#include "render/fx_renderer/fx_renderer.h"
#include "render/fx_renderer/shaders.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/util/log.h>

enum { kProbeWidth = 4, kProbeHeight = 2 };

struct fx_backdrop_probe {
  struct fx_renderer* renderer; // NULL once the renderer is gone
  struct wl_list link;          // fx_renderer.backdrop_probes
  GLuint texture;
  GLuint framebuffer;
  EGLSyncKHR sync;
  bool linear;
};

static const GLchar probe_vert_src[] = "attribute vec2 pos;\n"
                                       "uniform vec4 src;\n"
                                       "varying vec2 uv;\n"
                                       "void main() {\n"
                                       "  uv = src.xy + pos * src.zw;\n"
                                       "  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
                                       "}\n";

static const GLchar probe_frag_src[] = "precision mediump float;\n"
                                       "uniform sampler2D tex;\n"
                                       "varying vec2 uv;\n"
                                       "void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); }\n";

static struct {
  GLuint program;
  GLint pos;
  GLint src;
  GLint tex;
  struct fx_renderer* renderer;
} probe_shader;

static bool ensure_program(struct fx_renderer* renderer) {
  if (probe_shader.renderer == renderer && probe_shader.program != 0) {
    return true;
  }
  probe_shader.program = 0;
  probe_shader.renderer = NULL;
  GLuint vert = compile_shader(GL_VERTEX_SHADER, probe_vert_src);
  GLuint frag = vert != 0 ? compile_shader(GL_FRAGMENT_SHADER, probe_frag_src) : 0;
  if (frag == 0) {
    glDeleteShader(vert);
    return false;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vert);
  glAttachShader(program, frag);
  glLinkProgram(program);
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    wlr_log(WLR_ERROR, "backdrop probe: shader link failed");
    glDeleteProgram(program);
    return false;
  }
  probe_shader.program = program;
  probe_shader.pos = glGetAttribLocation(program, "pos");
  probe_shader.src = glGetUniformLocation(program, "src");
  probe_shader.tex = glGetUniformLocation(program, "tex");
  probe_shader.renderer = renderer;
  return true;
}

static void release_gl(struct fx_backdrop_probe* probe) {
  if (probe->sync != EGL_NO_SYNC_KHR) {
    wlr_egl_destroy_sync(probe->renderer->egl, probe->sync);
    probe->sync = EGL_NO_SYNC_KHR;
  }
  if (probe->framebuffer != 0) {
    glDeleteFramebuffers(1, &probe->framebuffer);
    probe->framebuffer = 0;
  }
  if (probe->texture != 0) {
    glDeleteTextures(1, &probe->texture);
    probe->texture = 0;
  }
}

struct fx_backdrop_probe* fx_backdrop_probe_create(struct wlr_renderer* wlr_renderer) {
  if (wlr_renderer == NULL || !wlr_renderer_is_fx(wlr_renderer)) {
    return NULL;
  }
  struct fx_renderer* renderer = fx_get_renderer(wlr_renderer);
  if (renderer->egl->procs.eglCreateSyncKHR == NULL || renderer->egl->eglClientWaitSyncKHR == NULL) {
    return NULL;
  }
  struct fx_backdrop_probe* probe = calloc(1, sizeof(*probe));
  if (probe == NULL) {
    return NULL;
  }
  probe->renderer = renderer;
  probe->sync = EGL_NO_SYNC_KHR;
  wl_list_insert(&renderer->backdrop_probes, &probe->link);
  return probe;
}

void fx_backdrop_probe_destroy(struct fx_backdrop_probe* probe) {
  if (probe == NULL) {
    return;
  }
  if (probe->renderer != NULL) {
    struct wlr_egl_context saved;
    wlr_egl_make_current(probe->renderer->egl, &saved);
    release_gl(probe);
    wlr_egl_restore_context(&saved);
    wl_list_remove(&probe->link);
  }
  free(probe);
}

struct wlr_renderer* fx_backdrop_probe_renderer(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->renderer != NULL ? &probe->renderer->wlr_renderer : NULL;
}

bool fx_backdrop_probe_pending(const struct fx_backdrop_probe* probe) {
  return probe != NULL && probe->renderer != NULL && probe->sync != EGL_NO_SYNC_KHR;
}

void fx_backdrop_probe_capture(
    struct fx_backdrop_probe* probe, GLuint texture, GLenum target, int texture_width, int texture_height,
    const struct wlr_box* box, bool linear
) {
  if (probe == NULL || probe->renderer == NULL || target != GL_TEXTURE_2D || texture_width <= 0
      || texture_height <= 0 || box->width <= 0 || box->height <= 0 || !ensure_program(probe->renderer)) {
    return;
  }
  struct wlr_egl* egl = probe->renderer->egl;
  if (probe->texture == 0) {
    glGenTextures(1, &probe->texture);
    glBindTexture(GL_TEXTURE_2D, probe->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kProbeWidth, kProbeHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &probe->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, probe->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, probe->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      wlr_log(WLR_ERROR, "backdrop probe: incomplete framebuffer");
      release_gl(probe);
      return;
    }
  }
  // The caller skips a capture while one is in flight; a node redrawn every frame would otherwise never finish one.
  if (probe->sync != EGL_NO_SYNC_KHR) {
    return;
  }

  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  glBindFramebuffer(GL_FRAMEBUFFER, probe->framebuffer);
  glViewport(0, 0, kProbeWidth, kProbeHeight);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glUseProgram(probe_shader.program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(probe_shader.tex, 0);
  // Each probe pixel center samples one point of a 4x2 grid over the box; the grid is symmetric in y, so the
  // texture's row order does not move it.
  glUniform4f(
      probe_shader.src, (float)box->x / (float)texture_width, (float)box->y / (float)texture_height,
      (float)box->width / (float)texture_width, (float)box->height / (float)texture_height
  );
  static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(probe_shader.pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(probe_shader.pos);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(probe_shader.pos);
  if (blend) {
    glEnable(GL_BLEND);
  }
  if (scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

  probe->sync = egl->procs.eglCreateSyncKHR(egl->display, EGL_SYNC_FENCE_KHR, NULL);
  probe->linear = linear;
}

bool fx_backdrop_probe_poll(struct fx_backdrop_probe* probe, float* luminance) {
  if (!fx_backdrop_probe_pending(probe)) {
    return false;
  }
  struct wlr_egl* egl = probe->renderer->egl;
  struct wlr_egl_context saved;
  if (!wlr_egl_make_current(egl, &saved)) {
    // Without a context the sample cannot be read; drop it so the caller stops re-checking.
    wlr_egl_destroy_sync(egl, probe->sync);
    probe->sync = EGL_NO_SYNC_KHR;
    return false;
  }
  bool done = false;
  const EGLint status = egl->eglClientWaitSyncKHR(egl->display, probe->sync, 0, 0);
  if (status == EGL_CONDITION_SATISFIED_KHR) {
    wlr_egl_destroy_sync(egl, probe->sync);
    probe->sync = EGL_NO_SYNC_KHR;
    GLubyte pixels[kProbeWidth * kProbeHeight * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, probe->framebuffer);
    glReadPixels(0, 0, kProbeWidth, kProbeHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    double sum = 0.0;
    for (int i = 0; i < kProbeWidth * kProbeHeight; i++) {
      const GLubyte* p = &pixels[i * 4];
      sum += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
    }
    double mean = sum / (kProbeWidth * kProbeHeight);
    *luminance = (float)(probe->linear ? pow(mean, 1.0 / 2.2) : mean);
    done = true;
  } else if (status == EGL_FALSE) {
    wlr_egl_destroy_sync(egl, probe->sync);
    probe->sync = EGL_NO_SYNC_KHR;
  }
  wlr_egl_restore_context(&saved);
  return done;
}

void fx_backdrop_probes_release(struct fx_renderer* renderer) {
  struct fx_backdrop_probe *probe, *tmp;
  wl_list_for_each_safe(probe, tmp, &renderer->backdrop_probes, link) {
    release_gl(probe);
    wl_list_remove(&probe->link);
    wl_list_init(&probe->link);
    probe->renderer = NULL;
  }
  if (probe_shader.renderer == renderer) {
    glDeleteProgram(probe_shader.program);
    probe_shader.program = 0;
    probe_shader.renderer = NULL;
  }
}
