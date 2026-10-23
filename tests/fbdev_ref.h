/*
 * fbdev's blend and clear twice in one translation unit: the code in
 * src/video/fbdev_render.c, and as ref_fbdev_display_blend and
 * ref_fbdev_display_clear the reference it must match byte for byte.
 *
 * tests/fbdev_render_ref.c is a verbatim copy of src/video/fbdev_render.c at
 * e083923, the single blend loop that every depth went through before the
 * split into one loop per depth. It is the oracle for test_fbdev_blend and the
 * "before" of bench_fbdev; it is not built into kmscon.
 */

#include "../src/video/fbdev_render.c"
#undef LOG_SUBSYSTEM

#define clamp_value ref_clamp_value
#define xrgb32_to_device ref_xrgb32_to_device
#define write_24bit ref_write_24bit
#define blend_mask ref_blend_mask
#define blend_argb ref_blend_argb
#define blend_px ref_blend_px
#define write_px ref_write_px
#define fbdev_display_blend ref_fbdev_display_blend
#define fbdev_display_clear ref_fbdev_display_clear
int fbdev_display_blend(struct display *disp, const struct video_blend_req *req);
int fbdev_display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b);
#include "fbdev_render_ref.c"
#undef clamp_value
#undef xrgb32_to_device
#undef write_24bit
#undef blend_mask
#undef blend_argb
#undef blend_px
#undef write_px
#undef fbdev_display_blend
#undef fbdev_display_clear
