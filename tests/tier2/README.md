# Tier 2: pixel comparison (ADR-301)

`make tier2` compares, `make tier2-golden` regenerates the goldens. `make test`
runs the same comparison as the `tier2_pixels` meson test, which is reported as
skipped when the freetype font backend or the drm2d video backend is disabled.

What the goldens cover: libtsm parsing bytes, `kmscon_text_draw`, the bbulk
renderer and the freetype backend, drawing through the real `drm2d_display_blend`
and `drm2d_display_clear` into an XRGB8888 buffer. The fbdev and drm3d/gltex
paths are not covered; a second pass through `fbdev_display_blend` with its own
goldens would be the way to add fbdev.

The goldens depend on the pinned fonts (`tests/fonts`), the libtsm commit in
`subprojects/libtsm.wrap`, and the FreeType in the dev image.

`NAME.tol` beside a golden sets that case's tolerance (`max_delta=`,
`max_pixels=`, `mask=x0,y0,x1,y1`); no sidecar means exact. Statuses in
`fork/build/tier2-report.json`: pass, near, fail, missing, error. The report
is deleted at the start of each run and always rewritten.

Each screen in `tests/test_pixels.c` carries a `desc`: what it shows and what
correct looks like. The renderer writes it beside the image as `NAME.txt`, and
the report carries it as the case's `description`, the caption on the
conformance run page. A new screen gets one.
