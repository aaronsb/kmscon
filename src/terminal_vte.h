/*
 * kmscon - A terminal's vte and its place in the image budget
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

/*
 * The part of terminal_new that makes a terminal's vte on its screen, sets it
 * from the configuration, and adds the terminal to the image budget (ADR-103),
 * and the decisions terminal.c makes for the vte's input modes: whether a key
 * repeat goes through (DECARM). It is apart from terminal.c so that a test
 * drives its failures and those decisions.
 */

#ifndef KMSCON_TERMINAL_VTE_H
#define KMSCON_TERMINAL_VTE_H

#include <libtsm.h>
#include "config.h"
#include "render/image_budget.h"

/*
 * Makes the vte on @con into @vte, sets it from @conf, and adds @images to
 * the budget. Returns 0, or a negative error with no vte made and nothing in
 * the budget; @con is the caller's either way. After 0, the unwind of any
 * later step is image_budget_free_terminal(@images), which drops the vte and
 * the caller's reference on @con.
 */
int terminal_vte_new(struct tsm_vte **vte, struct image_budget_terminal *images,
		     struct tsm_screen *con, struct kmscon_conf_t *conf, tsm_vte_write_cb write_cb,
		     void *data);

/*
 * Whether a key repeat is dropped before it reaches @vte: true when @repeat
 * (the event came from the input layer's repeat timer, not the keyboard) and
 * the program has reset DECARM, mode 8 (xterm ctlseqs). kmscon makes a held
 * key's repeats, so it is the one that stops them; its own key bindings are
 * matched before this and keep repeating.
 */
bool terminal_vte_drop_repeat(struct tsm_vte *vte, bool repeat);

#endif /* KMSCON_TERMINAL_VTE_H */
