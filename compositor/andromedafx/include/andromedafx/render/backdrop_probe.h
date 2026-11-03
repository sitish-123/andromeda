#ifndef ANDROMEDAFX_RENDER_BACKDROP_PROBE_H
#define ANDROMEDAFX_RENDER_BACKDROP_PROBE_H

#include <stdbool.h>

struct wlr_renderer;

/**
 * Measures the mean luminance of a blurred backdrop without stalling the GPU. A blur pass that carries a probe draws a
 * 4x2 sample grid of its blurred region into the probe and fences it; fx_backdrop_probe_poll() reads the samples only
 * after the fence signalled.
 */
struct fx_backdrop_probe;

struct fx_backdrop_probe* fx_backdrop_probe_create(struct wlr_renderer* renderer);
void fx_backdrop_probe_destroy(struct fx_backdrop_probe* probe);

/** The renderer the probe samples with; NULL after that renderer was destroyed. */
struct wlr_renderer* fx_backdrop_probe_renderer(const struct fx_backdrop_probe* probe);

/** True while a captured sample waits for its fence. */
bool fx_backdrop_probe_pending(const struct fx_backdrop_probe* probe);

/**
 * Never blocks. Returns true and sets luminance (0 to 1, display-encoded) when a captured sample finished.
 * Makes the renderer's EGL context current and restores the previous one.
 */
bool fx_backdrop_probe_poll(struct fx_backdrop_probe* probe, float* luminance);

#endif
