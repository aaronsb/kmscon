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

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include "image_identity.h"
#include "shl/log.h"

/* the build's choice, which a test may make for itself */
#ifndef IMAGE_IDENTITY_SD_LOGIN
#ifdef BUILD_ENABLE_SD_LOGIN
#define IMAGE_IDENTITY_SD_LOGIN 1
#else
#define IMAGE_IDENTITY_SD_LOGIN 0
#endif
#endif

#if IMAGE_IDENTITY_SD_LOGIN
#include <systemd/sd-login.h>
#endif

#define LOG_SUBSYSTEM "image_identity"

#if IMAGE_IDENTITY_SD_LOGIN
/* logind's uid for @child's session; pam_systemd registered it at login */
static int logind_uid(pid_t child, uid_t *uid)
{
	char *session = NULL;
	int ret;

	if (child <= 0)
		return -ESRCH;
	ret = sd_pid_get_session(child, &session);
	if (ret < 0)
		return ret;
	ret = sd_session_get_uid(session, uid);
	free(session);
	return ret < 0 ? ret : 0;
}
#endif

#if IMAGE_IDENTITY_SD_LOGIN
/*
 * Logs a disagreement between the pty's owner and logind once per pair: the
 * identity is found again for every request, and a session left in that
 * state would otherwise log it on every image.
 */
static void log_disagreement(uid_t owner, uid_t logind)
{
	static bool logged;
	static uid_t last_owner, last_logind;

	if (logged && owner == last_owner && logind == last_logind)
		return;
	logged = true;
	last_owner = owner;
	last_logind = logind;
	log_warning("the pty belongs to uid %u, but logind's session is uid %u's; "
		    "the image helper runs as %u",
		    owner, logind, logind);
}
#endif

int image_identity_uid(pid_t child, int owner_ret, uid_t owner, uid_t *uid, bool *files)
{
#if IMAGE_IDENTITY_SD_LOGIN
	uid_t logind;

	if (!logind_uid(child, &logind)) {
		if (!owner_ret && owner != logind)
			log_disagreement(owner, logind);
		*uid = logind;
		*files = true;
		return 0;
	}
	/*
	 * sd_get_seats counts logind's seats: 0 when logind is not running
	 * (no /run/systemd/seats, or an empty one), more when it is, below 0
	 * on an error. With logind running and the process in none of its
	 * sessions there is no session user yet, so the file media are
	 * refused; an error refuses them too, failing closed.
	 */
	*files = sd_get_seats(NULL) == 0;
#else
	(void)child;
	*files = true;
#endif
	if (owner_ret)
		return owner_ret;
	*uid = owner;
	return 0;
}
