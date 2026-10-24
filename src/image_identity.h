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
 * The user the image helper runs as (ADR-103, The image helper, Identity),
 * the session's user and never the foreground process's, in one of three
 * modes:
 *
 * - Built with sd-login (the sd_login option) and the process kmscon started
 *   in a logind session of its own, of a login class ("user", "user-early",
 *   "user-light" or "user-early-light"): that session's uid,
 *   the identity PAM authenticated, root included. A session it shares with
 *   kmscon is one it inherited (kmscon started inside someone's session, or
 *   kmscon's greeter session), where a login on the pty creates none of its
 *   own, so it does not count. When the pty's owner disagrees, logind's
 *   answer stands and the disagreement is logged.
 * - logind running, or its state unknown (sd_get_seats failing), and the
 *   process in no session of its own, or its session's class or uid not
 *   found: before login. The
 *   file media (t=f, t=t, t=s) are refused without starting the helper, and
 *   inline PNG and zlib decode as the system user kmscon-image, else nobody,
 *   else are refused.
 * - Built without sd-login, or no logind running (sd_get_seats 0): the owner
 *   of the session's pty, as /bin/login sets it. Never root by fallback: a
 *   root owner is treated as before login.
 *
 * A kmscon not running as root whose real, effective or saved gid, or any
 * supplementary group, is 0 (a setgid-root install) is treated as before
 * login in every mode: its helper would keep that group.
 *
 * The pre-login user is looked up once, when first needed, and kept, so a
 * slow network name service cannot stall the login prompt on every request.
 * A candidate whose lookup fails, or that resolves to uid 0 or gid 0, is
 * passed over.
 */

#ifndef KMSCON_IMAGE_IDENTITY_H
#define KMSCON_IMAGE_IDENTITY_H

#include <stdbool.h>
#include <sys/types.h>

/* the pre-login user, then its fallback */
#define IMAGE_IDENTITY_PRELOGIN_USER "kmscon-image"
#define IMAGE_IDENTITY_PRELOGIN_FALLBACK "nobody"

struct image_user {
	uid_t uid;
	/*
	 * false: the uid's primary gid and supplementary groups, looked up
	 * when the helper starts; true: @gid alone, with no supplementary
	 * group, as the pre-login user runs
	 */
	bool gid_only;
	gid_t gid;
};

/*
 * The user a request from the pty's @child runs as; @owner_ret is 0 when
 * @owner is the pty's owner, else a negative error. @file says the request
 * names a file, temporary file or shared memory object, a discard among
 * them. Returns 0 with @user set; -EACCES when the file media are refused
 * (before login, or a root owner by fallback); -ENOENT when an inline request
 * before login finds neither pre-login user; or @owner_ret when the pty's
 * owner is needed and unknown.
 */
int image_identity_find(pid_t child, int owner_ret, uid_t owner, bool file,
			struct image_user *user);

#endif /* KMSCON_IMAGE_IDENTITY_H */
