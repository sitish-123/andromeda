#ifndef ANDROMEDAFX_RENDER_FRAME_STATS_H
#define ANDROMEDAFX_RENDER_FRAME_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Effect work of the frames built since the last take; the frame log (ANDROMEDA_FRAME_LOG) prints it per frame.
// Pixel counts are render-target pixels.
struct fx_frame_stats {
  uint32_t blurs;             // live or optimized blur nodes drawn
  uint64_t blur_px;           // area each blur read (damage expanded by the kernel), summed
  uint32_t lens;              // lens copies
  uint64_t lens_px;
  uint32_t backdrop_rebuilds; // animation_backdrop rebuilds inside captured groups
  uint64_t backdrop_px;
  uint32_t fp16_passes;       // passes through the FP16 blend buffer (colour transform)
  uint32_t rects;             // solid-fill boxes drawn as quads (not glClear)
  uint64_t rect_px;
};

struct fx_frame_stats fx_frame_stats_take(void);

#ifdef __cplusplus
}
#endif

#endif
