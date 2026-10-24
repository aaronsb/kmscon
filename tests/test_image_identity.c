/*
 * The image helper's user (src/image_identity.c, ADR-103's Identity), built
 * twice: with sd-login, whose three calls this file stands in for, and
 * without. It stands in for getpwnam_r too, to choose which pre-login users
 * exist.
 *
 * - In a logind session of the child's own, of class "user": logind's uid,
 *   root included, with its groups, and a pty owner that disagrees is logged.
 *   A session shared with kmscon, inherited, or of another class is none.
 * - logind running or its state unknown, and no session uid: before login.
 *   File media are refused; inline media run as kmscon-image, then nobody,
 *   each passed over when absent, failing or uid or gid 0, then are refused.
 *   The pre-login user has its gid alone, and is looked up once.
 * - Without sd-login, or no logind running: the pty's owner, except that a
 *   root owner is handled as before login (never root by fallback).
 *
 * The pre-login lookup is kept for the process's life, so each case that
 * reaches it runs in a child of its own.
 */

#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "image_identity.h"

/* the users getpwnam_r knows; uid 0 marks one absent, ERANGE one whose lookup fails */
static struct {
	const char *name;
	uid_t uid;
	gid_t gid;
	int error;
	bool present;
} users[2] = {
	{"kmscon-image", 990, 991, 0, true},
	{"nobody", 65534, 65534, 0, true},
};
static unsigned int lookups;

/* kmscon's own ids, stood in for: a non-root kmscon in gid 1000 unless a case says otherwise */
static uid_t own_euid = 1000;
static gid_t own_gids[3] = {1000, 1000, 1000};
static gid_t own_groups[2] = {1000, 1000};

uid_t geteuid(void)
{
	return own_euid;
}

int getresgid(gid_t *r, gid_t *e, gid_t *s)
{
	*r = own_gids[0];
	*e = own_gids[1];
	*s = own_gids[2];
	return 0;
}

int getgroups(int size, gid_t list[])
{
	assert(size >= 2);
	memcpy(list, own_groups, sizeof(own_groups));
	return 2;
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t size, struct passwd **res)
{
	unsigned int i;

	(void)buf;
	(void)size;
	lookups++;
	*res = NULL;
	for (i = 0; i < 2; i++) {
		if (strcmp(name, users[i].name))
			continue;
		if (users[i].error)
			return users[i].error;
		if (!users[i].present)
			return 0;
		*pw = (struct passwd){.pw_name = (char *)users[i].name,
				      .pw_uid = users[i].uid,
				      .pw_gid = users[i].gid};
		*res = pw;
		return 0;
	}
	return 0;
}

#if IMAGE_IDENTITY_SD_LOGIN
#include <systemd/sd-login.h>

/* logind's answer: a session for @session_pid, whose user is session_uid */
static pid_t session_pid;
static uid_t session_uid;
static int session_error;	/* sd_pid_get_session's, for every other pid */
static int uid_error;		/* sd_session_get_uid's */
static const char *own_session; /* kmscon's own, or NULL for none */
static int own_error;		/* sd_pid_get_session's for kmscon itself */
static const char *session_class = "user";
static int class_error; /* sd_session_get_class's */
static unsigned int asked;
/* libsystemd's answer: seats counted, 0 when logind is not running, or an error */
static int seats;

int sd_get_seats(char ***out)
{
	assert(!out);
	return seats;
}

int sd_pid_get_session(pid_t pid, char **session)
{
	if (pid == getpid()) {
		if (own_error)
			return own_error;
		if (!own_session)
			return -ENODATA;
		*session = strdup(own_session);
		return 0;
	}
	asked++;
	if (pid != session_pid)
		return session_error;
	*session = strdup("c1");
	return 0;
}

int sd_session_get_class(const char *session, char **class)
{
	assert(!strcmp(session, "c1"));
	if (class_error)
		return class_error;
	*class = strdup(session_class);
	return 0;
}

int sd_session_get_uid(const char *session, uid_t *uid)
{
	assert(!strcmp(session, "c1"));
	if (uid_error)
		return uid_error;
	*uid = session_uid;
	return 0;
}
#endif

static int find(pid_t child, int owner_ret, uid_t owner, bool file, struct image_user *user)
{
	*user = (struct image_user){.uid = 12345, .gid = 12345};
	return image_identity_find(child, owner_ret, owner, file, user);
}

/* a session user: @uid with its own groups */
static void expect_user(pid_t child, int owner_ret, uid_t owner, bool file, uid_t uid)
{
	struct image_user user;

	assert(find(child, owner_ret, owner, file, &user) == 0);
	assert(user.uid == uid && !user.gid_only);
}

/*
 * Before login, in a child of its own: file media refused with no lookup,
 * inline media as the pre-login user @uid and @gid with no other group, or
 * refused when @uid is 0, the lookup made once
 */
static void expect_prelogin(pid_t child, int owner_ret, uid_t owner, uid_t uid, gid_t gid)
{
	struct image_user user;
	unsigned int n;
	int status;
	pid_t pid;

	fflush(stdout);
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		lookups = 0;
		assert(find(child, owner_ret, owner, true, &user) == -EACCES && !lookups);
		if (!uid) {
			assert(find(child, owner_ret, owner, false, &user) == -ENOENT);
		} else {
			assert(find(child, owner_ret, owner, false, &user) == 0);
			assert(user.uid == uid && user.gid == gid && user.gid_only);
		}
		n = lookups;
		assert(n >= 1 && n <= 2);
		find(child, owner_ret, owner, false, &user);
		find(child, owner_ret, owner, true, &user);
		assert(lookups == n);
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

/* the candidates for a root owner by fallback, or before login as @child */
static void test_candidates(pid_t child, uid_t owner)
{
	/* kmscon-image */
	expect_prelogin(child, 0, owner, 990, 991);
	/* absent, its lookup failing, uid 0, gid 0: nobody */
	users[0].present = false;
	expect_prelogin(child, 0, owner, 65534, 65534);
	users[0].present = true;
	users[0].error = ERANGE;
	expect_prelogin(child, 0, owner, 65534, 65534);
	users[0].error = 0;
	users[0].uid = 0;
	expect_prelogin(child, 0, owner, 65534, 65534);
	users[0].uid = 990;
	users[0].gid = 0;
	expect_prelogin(child, 0, owner, 65534, 65534);
	users[0].gid = 991;
	/* neither: refused; nobody passed over for the same reasons */
	users[0].present = false;
	users[1].present = false;
	expect_prelogin(child, 0, owner, 0, 0);
	users[1].present = true;
	users[1].uid = 0;
	expect_prelogin(child, 0, owner, 0, 0);
	users[1].uid = 65534;
	users[1].gid = 0;
	expect_prelogin(child, 0, owner, 0, 0);
	users[1].gid = 65534;
	users[0].present = true;
}

/*
 * A non-root kmscon holding gid 0, real, effective, saved or supplementary,
 * is before login whatever else the mode would give; a root kmscon is not
 */
static void test_gid_zero(pid_t child, uid_t owner, uid_t uid)
{
	unsigned int i;

	for (i = 0; i < 3; i++) {
		own_gids[i] = 0;
		expect_prelogin(child, 0, owner, 990, 991);
		own_gids[i] = 1000;
	}
	own_groups[1] = 0;
	expect_prelogin(child, 0, owner, 990, 991);
	own_euid = 0;
	expect_user(child, 0, owner, true, uid);
	own_euid = 1000;
	own_groups[1] = 1000;
	expect_user(child, 0, owner, true, uid);
}

int main(void)
{
	struct image_user user;

#if IMAGE_IDENTITY_SD_LOGIN
	session_pid = 77;
	session_uid = 4242;
	session_error = -ENXIO;
	seats = 1;

	/* logind's uid, whether the pty's owner agrees, disagrees or is unknown */
	expect_user(77, 0, 4242, true, 4242);
	expect_user(77, 0, 0, true, 4242);
	expect_user(77, -EIO, 0, false, 4242);
	/* root that logind authenticated keeps the helper as root */
	session_uid = 0;
	expect_user(77, 0, 0, true, 0);
	expect_user(77, 0, 1000, false, 0);
	session_uid = 4242;
	/* kmscon in a session of its own, another than the child's: still the child's */
	own_session = "c9";
	expect_user(77, 0, 4242, true, 4242);
	/* even a session user is before login when a non-root kmscon holds gid 0 */
	test_gid_zero(77, 4242, 4242);

	/*
	 * A session the child inherited from kmscon (kmscon started inside
	 * someone's session, or its greeter session) is no login of the
	 * child's: before login, whoever owns that session
	 */
	own_session = "c1";
	expect_prelogin(77, 0, 4242, 990, 991);
	session_uid = 0;
	expect_prelogin(77, 0, 1000, 990, 991);
	session_uid = 4242;
	own_session = NULL;
	/* kmscon's own session unknown for another reason: failing closed */
	own_error = -EIO;
	expect_prelogin(77, 0, 4242, 990, 991);
	own_error = 0;
	/* only class "user" is a login: a greeter's, or a class not found, is not */
	session_class = "greeter";
	expect_prelogin(77, 0, 4242, 990, 991);
	session_class = "user";
	class_error = -EIO;
	expect_prelogin(77, 0, 4242, 990, 991);
	class_error = 0;

	/* logind running and the process in no session (the login prompt): before login */
	expect_prelogin(78, 0, 0, 990, 991);
	expect_prelogin(78, 0, 1000, 990, 991);
	expect_prelogin(78, -EIO, 1000, 990, 991);
	test_candidates(78, 1000);
	/* no child: logind is not asked, and with logind running, before login */
	asked = 0;
	expect_prelogin(0, 0, 1000, 990, 991);
	assert(find(0, 0, 1000, true, &user) == -EACCES && !asked);
	/* sd-login failing otherwise, with logind running: before login */
	session_error = -EIO;
	expect_prelogin(78, 0, 1000, 990, 991);
	session_error = -ENXIO;
	uid_error = -EIO;
	expect_prelogin(77, 0, 1000, 990, 991);
	uid_error = 0;

	/* logind's seats unknown: counted as running, failing closed */
	seats = -EIO;
	expect_prelogin(78, 0, 1000, 990, 991);
	uid_error = -ENOENT;
	expect_prelogin(77, 0, 1000, 990, 991);
	uid_error = 0;

	/* no logind running: the pty's owner, file media allowed */
	seats = 0;
	expect_user(78, 0, 1000, true, 1000);
	expect_user(78, 0, 1000, false, 1000);
	assert(find(78, -EIO, 1000, false, &user) == -EIO);
	/* never root by fallback: a root owner is before login, at the prompt or after a root login
	 */
	expect_prelogin(78, 0, 0, 990, 991);
	test_candidates(78, 0);
	printf("test_image_identity: ok, with sd-login\n");
#else
	/* the pty's owner, file media allowed; no session is looked for */
	expect_user(77, 0, 1000, true, 1000);
	expect_user(77, 0, 1000, false, 1000);
	assert(find(77, -EIO, 1000, false, &user) == -EIO);
	assert(find(77, -EIO, 1000, true, &user) == -EIO);
	/* never root by fallback: a root owner is before login, at the prompt or after a root login
	 */
	expect_prelogin(77, 0, 0, 990, 991);
	test_candidates(77, 0);
	/* a non-root kmscon holding gid 0: before login, the pty's owner aside */
	test_gid_zero(77, 1000, 1000);
	printf("test_image_identity: ok, without sd-login\n");
#endif
	return 0;
}
