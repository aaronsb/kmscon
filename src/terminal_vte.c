/*
 * kmscon - A terminal's vte, its place in the image budget, and its input modes
 *
 * Copyright (c) 2026 the kmscon fork authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <libtsm.h>
#include "shl/log.h"
#include "terminal_vte.h"

#define LOG_SUBSYSTEM "terminal"

int terminal_vte_new(struct tsm_vte **vte, struct image_budget_terminal *images,
		     struct tsm_screen *con, struct kmscon_conf_t *conf, tsm_vte_write_cb write_cb,
		     void *data)
{
	struct tsm_vte *v;
	int ret;

	ret = tsm_vte_new(&v, con, write_cb, data, log_llog, NULL);
	if (ret)
		return ret;

	/* images (ADR-103): the process's budget, and sixel and kitty graphics by
	 * image_budget.h's rule */
	if (tsm_vte_set_graphic_max_size(v, conf->max_graphic_size.width,
					 conf->max_graphic_size.height) ||
	    tsm_vte_set_sixel_registers(v, conf->num_color_registers,
					conf->private_color_registers))
		log_warning("cannot apply the sixel settings; libtsm's defaults stand");
	ret = image_budget_add_terminal(images, con, v, conf->sixel, conf->kitty_graphics);
	if (ret)
		goto err_vte;

	tsm_vte_set_backspace_sends_delete(v, conf->backspace_delete);
	/* DECRQM ?12 tells the truth only if libtsm knows the cursor can't blink */
	tsm_vte_set_cursor_blink(v, conf->blink);

	ret = tsm_vte_set_palette(v, conf->palette);
	if (ret)
		goto err_images;

	ret = tsm_vte_set_custom_palette(v, conf->custom_palette);
	if (ret)
		goto err_images;

	ret = tsm_vte_set_width_method(v, conf->grapheme_width);
	if (ret)
		goto err_images;

	*vte = v;
	return 0;

err_images:
	/* before the vte goes: leaving the budget turns images off on it */
	image_budget_remove_terminal(images);
err_vte:
	tsm_vte_unref(v);
	return ret;
}

/* DECRQM's value for a mode that is reset (ctlseqs DECRPM) */
#define MODE_RESET 2

bool terminal_vte_drop_repeat(struct tsm_vte *vte, bool repeat)
{
	return repeat && tsm_vte_get_mode(vte, true, 8) == MODE_RESET;
}

bool terminal_vte_focus(struct tsm_vte *vte, bool *last, bool focus)
{
	if (*last == focus)
		return false;
	*last = focus;
#ifdef TSM_VTE_HANDLE_FOCUS
	return tsm_vte_handle_focus(vte, focus);
#else
	/* a libtsm before mode 1004: nothing to report to */
	(void)vte;
	return false;
#endif
}

unsigned int terminal_vte_pointer(int32_t pos, unsigned int cells, unsigned int cell_size)
{
	unsigned int size = cells * cell_size;

	if (pos <= 0 || !size)
		return 0;
	if ((unsigned int)pos >= size)
		return size - 1;
	return pos;
}
