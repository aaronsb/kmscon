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
#include <pwd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * sd_get_seats counts the entries of /run/systemd/seats: 0 when logind is
 * not running, more when it is, below 0 on an error. An error counts as
 * running, so that it fails closed.
 */
static bool logind_running(void)
{
	return sd_get_seats(NULL) != 0;
}

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

/* @name's uid and gid, unless its lookup fails or either is 0 */
static bool prelogin_candidate(const char *name, uid_t *uid, gid_t *gid)
{
	struct passwd pw, *res = NULL;
	char buf[4096];

	if (getpwnam_r(name, &pw, buf, sizeof(buf), &res) || !res)
		return false;
	if (!pw.pw_uid || !pw.pw_gid) {
		log_warning("user %s is uid %u, gid %u: not used for images before login", name,
			    pw.pw_uid, pw.pw_gid);
		return false;
	}
	*uid = pw.pw_uid;
	*gid = pw.pw_gid;
	return true;
}

/* kmscon-image, else nobody, looked up the first time and kept, a miss included */
static int prelogin_user(struct image_user *user)
{
	static bool resolved, found;
	static uid_t uid;
	static gid_t gid;

	if (!resolved) {
		resolved = true;
		found = prelogin_candidate(IMAGE_IDENTITY_PRELOGIN_USER, &uid, &gid) ||
			prelogin_candidate(IMAGE_IDENTITY_PRELOGIN_FALLBACK, &uid, &gid);
		if (!found)
			log_warning("neither %s nor %s can decode images before login",
				    IMAGE_IDENTITY_PRELOGIN_USER, IMAGE_IDENTITY_PRELOGIN_FALLBACK);
	}
	if (!found)
		return -ENOENT;
	*user = (struct image_user){.uid = uid, .gid_only = true, .gid = gid};
	return 0;
}

/* before login, or a root owner by fallback: no file, and inline media as the pre-login user */
static int before_login(bool file, struct image_user *user)
{
	if (file)
		return -EACCES;
	return prelogin_user(user);
}

int image_identity_find(pid_t child, int owner_ret, uid_t owner, bool file, struct image_user *user)
{
#if IMAGE_IDENTITY_SD_LOGIN
	uid_t logind;

	if (!logind_uid(child, &logind)) {
		if (!owner_ret && owner != logind)
			log_disagreement(owner, logind);
		*user = (struct image_user){.uid = logind};
		return 0;
	}
	/* logind running and no session uid: no session user yet */
	if (logind_running())
		return before_login(file, user);
#else
	(void)child;
#endif
	if (owner_ret)
		return owner_ret;
	/* never root by fallback: a root pty is a login prompt's, or root's unauthenticated */
	if (!owner)
		return before_login(file, user);
	*user = (struct image_user){.uid = owner};
	return 0;
}
