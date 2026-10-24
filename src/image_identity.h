/*
 * kmscon - The image helper's user
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
 * The session's user, whom the image helper runs as (ADR-103, The image
 * helper, Identity). Built with sd-login (the sd_login option), it is
 * logind's uid for the session of the process kmscon started, the identity
 * PAM authenticated. Otherwise, or when that process is in no logind
 * session, it is the owner of the session's pty, as /bin/login sets it. The
 * pty's owner is file metadata a root process in the session can change, so
 * when the two disagree logind's answer stands and the disagreement is
 * logged.
 *
 * Before login, when logind runs but the process is in no session yet (the
 * login prompt), there is no session user, and the pty is still root's. The
 * file media are then refused (the operator's choice, "Refuse before
 * login"); inline images, which name no file, still go to the helper as the
 * pty's owner. Without sd-login, or with no logind running, the pty's owner
 * stands as before.
 */

#ifndef KMSCON_IMAGE_IDENTITY_H
#define KMSCON_IMAGE_IDENTITY_H

#include <stdbool.h>
#include <sys/types.h>

/*
 * The user for @child, the process kmscon started on the pty; @owner_ret is
 * 0 when @owner is the pty's owner, else a negative error. Returns 0 with
 * @uid set, and @files false when the file media (t=f, t=t, t=s) are refused
 * before login, or a negative error when neither source answers.
 */
int image_identity_uid(pid_t child, int owner_ret, uid_t owner, uid_t *uid, bool *files);

#endif /* KMSCON_IMAGE_IDENTITY_H */
