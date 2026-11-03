#ifndef RENDER_FX_RENDERER_BACKDROP_PROBE_H
#define RENDER_FX_RENDERER_BACKDROP_PROBE_H

#include "andromedafx/render/backdrop_probe.h"
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <wlr/util/box.h>

struct fx_renderer;

/** True when no sample is in flight and the first sample, or 100 ms since the last one, is due. */
bool fx_backdrop_probe_due(const struct fx_backdrop_probe* probe);

/**
 * Draws the 4x2 sample grid of box (texture px, top-left origin) from texture into the probe and fences it. linear:
 * the texture holds linear-light values. Restores viewport, blend and
 * scissor; the caller rebinds its framebuffer.
 */
void fx_backdrop_probe_capture(
    struct fx_backdrop_probe* probe, GLuint texture, GLenum target, int texture_width, int texture_height,
    const struct wlr_box* box, bool linear
);

/** Releases the GL objects of every probe of this renderer; the renderer's context is current. */
void fx_backdrop_probes_release(struct fx_renderer* renderer);

#endif
