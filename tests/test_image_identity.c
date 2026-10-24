/*
 * The image helper's user (src/image_identity.c, ADR-103's Identity), built
 * twice: with sd-login, whose two calls this file stands in for, and without.
 * With it, logind's uid for the pty's child stands, and a pty owner that
 * disagrees is logged; without it, or with the child in no logind session,
 * the pty's owner is the user.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_identity.h"

#if IMAGE_IDENTITY_SD_LOGIN
#include <systemd/sd-login.h>

/* logind's answer: a session for @session_pid, whose user is session_uid */
static pid_t session_pid;
static uid_t session_uid;
static unsigned int asked;

int sd_pid_get_session(pid_t pid, char **session)
{
	asked++;
	if (pid != session_pid)
		return -ENODATA;
	*session = strdup("c1");
	return 0;
}

int sd_session_get_uid(const char *session, uid_t *uid)
{
	assert(!strcmp(session, "c1"));
	*uid = session_uid;
	return 0;
}
#endif

int main(void)
{
	uid_t uid = 0;

#if IMAGE_IDENTITY_SD_LOGIN
	session_pid = 77;
	session_uid = 4242;

	/* logind's uid, whether the pty's owner agrees, disagrees or is unknown */
	assert(image_identity_uid(77, 0, 4242, &uid) == 0 && uid == 4242);
	assert(image_identity_uid(77, 0, 0, &uid) == 0 && uid == 4242);
	assert(image_identity_uid(77, -EIO, 0, &uid) == 0 && uid == 4242);
	/* no session: the pty's owner, or nothing */
	assert(image_identity_uid(78, 0, 1000, &uid) == 0 && uid == 1000);
	assert(image_identity_uid(78, -EIO, 1000, &uid) == -EIO);
	/* no child: logind is not asked */
	asked = 0;
	assert(image_identity_uid(0, 0, 1000, &uid) == 0 && uid == 1000 && !asked);
	printf("test_image_identity: ok, with sd-login\n");
#else
	/* the pty's owner alone; no session is looked for */
	assert(image_identity_uid(77, 0, 1000, &uid) == 0 && uid == 1000);
	assert(image_identity_uid(77, 0, 0, &uid) == 0 && uid == 0);
	assert(image_identity_uid(77, -EIO, 1000, &uid) == -EIO);
	printf("test_image_identity: ok, without sd-login\n");
#endif
	return 0;
}
