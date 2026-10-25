/*
 * A DRM mode's refresh rate (src/video/drm_refresh.h), which bounds how long
 * coalescing holds a draw back (ADR-201 step 6), from the timings of known
 * modes.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "video/drm_refresh.h"

static unsigned int refresh(uint32_t clock, uint16_t htotal, uint16_t vtotal, uint32_t flags,
			    uint16_t vscan, uint32_t vrefresh)
{
	drmModeModeInfo m;

	memset(&m, 0, sizeof(m));
	m.clock = clock;
	m.htotal = htotal;
	m.vtotal = vtotal;
	m.flags = flags;
	m.vscan = vscan;
	m.vrefresh = vrefresh;
	return drm_mode_refresh_mhz(&m);
}

int main(void)
{
	/* CEA-861 1920x1080 at 60 Hz, and at 60/1.001 Hz */
	assert(refresh(148500, 2200, 1125, 0, 0, 60) == 60000);
	assert(refresh(148352, 2200, 1125, 0, 0, 60) == 59940);
	/* 1920x1080i: two fields a frame */
	assert(refresh(74250, 2200, 1125, DRM_MODE_FLAG_INTERLACE, 0, 60) == 60000);
	/* VGA 640x480 timings scanned twice: 320x240 doublescan */
	assert(refresh(25175, 800, 525, DRM_MODE_FLAG_DBLSCAN, 0, 30) == 29970);
	/* each line scanned three times */
	assert(refresh(148500, 2200, 1125, 0, 3, 20) == 20000);
	/* 7680x2160 at 120 Hz (CVT-RB v2 totals) */
	assert(refresh(2116320, 7760, 2273, 0, 0, 120) == 119983);
	/* no totals: the rounded rate */
	assert(refresh(0, 0, 0, 0, 0, 75) == 75000);
	printf("test_drm_refresh: ok\n");
	return 0;
}
