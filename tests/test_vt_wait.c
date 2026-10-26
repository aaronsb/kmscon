/*
 * Waiting for the VT (#250). With --vt and --no-switchvt, kmscon waits for
 * its VT to become active before it scans for devices. The wait runs on the
 * event loop: until the VT is active, a signal is served, and SIGTERM leaves
 * the VT's keyboard and display modes as they were before kmscon opened it.
 * A VT active at the start ends the wait at once. A VT activated later ends
 * it, before the activation is reported, so the scan precedes the
 * activation as it does at startup. A hangup on the VT ends it with -EPIPE.
 *
 * The real backend's wait ends on the VT's acquire signal, SIGUSR1, sent
 * here by the test; a wait that returned 0 leaves nothing for a later
 * acquire to call. The libseat backend's ends when a poll from the loop
 * sees the VT active, and only then does it open the seat; a seat that does
 * not open fails the wait with its error, and the modes the enabled seat
 * set are restored on exit.
 *
 * A pseudo-terminal slave stands in for the VT, and ioctl is wrapped
 * (-Wl,--wrap=ioctl) to answer the VT and keyboard calls as the kernel
 * would. VT_WAITACTIVE on an inactive VT blocks in the kernel; the stand-in
 * fails the test there instead. Input and libseat are stood in for.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "shl/eloop.h"
#include "vt.h"

#ifdef BUILD_ENABLE_LIBSEAT
#include <libseat.h>
#endif

/* the kernel's VT state, as the wrapped ioctl reports and changes it */
static int fake_active;
static int fake_kbmode;
static int fake_kdmode;
static int fake_vtmode;

int __real_ioctl(int fd, unsigned long req, ...);
int __wrap_ioctl(int fd, unsigned long req, ...);

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	unsigned long arg;
	va_list ap;

	va_start(ap, req);
	arg = va_arg(ap, unsigned long);
	va_end(ap);

	switch (req) {
	case VT_GETSTATE:
		((struct vt_stat *)arg)->v_active = fake_active;
		return 0;
	case VT_WAITACTIVE:
		if ((int)arg == fake_active)
			return 0;
		fprintf(stderr, "VT_WAITACTIVE blocks the caller until VT %d is active\n",
			(int)arg);
		exit(1);
	case KDGKBMODE:
		*(int *)arg = fake_kbmode;
		return 0;
	case KDSKBMODE:
		fake_kbmode = arg;
		return 0;
	case KDSETMODE:
		fake_kdmode = arg;
		return 0;
	case VT_SETMODE:
		fake_vtmode = ((struct vt_mode *)arg)->mode;
		return 0;
	case VT_RELDISP:
	case VT_ACTIVATE:
		return 0;
	default:
		return __real_ioctl(fd, req, arg);
	}
}

/* input, which the VT backends only register with and wake */
void input_ref(struct input *input) {}
void input_unref(struct input *input) {}
int input_register_key_cb(struct input *input, input_key_cb cb, void *data)
{
	return 0;
}
void input_unregister_key_cb(struct input *input, input_key_cb cb, void *data) {}
void input_set_device_ops(struct input *input, uterm_open_cb open_cb, uterm_close_cb close_cb,
			  void *data)
{
}
void input_sleep(struct input *input) {}
void input_wake_up(struct input *input) {}

static int input_dummy;
#define INPUT ((struct input *)&input_dummy)

/*
 * what happened, in order: W the wait ended, A activated, D deactivated,
 * H hangup, T SIGTERM
 */
static char events[16];
static int wait_error;

static void event(char c)
{
	size_t n = strlen(events);

	assert(n + 1 < sizeof(events));
	events[n] = c;
	events[n + 1] = 0;
}

static void vt_activate(struct uterm_vt *vt, void *data)
{
	event('A');
}

static int vt_deactivate(struct uterm_vt *vt, bool force, void *data)
{
	event('D');
	return 0;
}

static void vt_hup(struct uterm_vt *vt, void *data)
{
	event('H');
}

static struct uterm_vt_cb vt_cb = {
	.activate = vt_activate,
	.deactivate = vt_deactivate,
	.hup = vt_hup,
};

static void waited(struct uterm_vt *vt, int error, void *data)
{
	wait_error = error;
	event('W');
}

/* kmscon's handler exits the loop; this one leaves it running to go on */
static void sig_term(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	event('T');
}

/* a pseudo-terminal: its slave's path in @path, its number returned */
static int open_pty(int *master, char *path, size_t size)
{
	struct stat st;

	*master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	assert(*master >= 0);
	assert(grantpt(*master) == 0 && unlockpt(*master) == 0);
	assert(ptsname_r(*master, path, size) == 0);
	assert(stat(path, &st) == 0);
	return minor(st.st_rdev);
}

static struct ev_eloop *eloop;
static int master;
static char path[64];
static int num;

/* a fresh loop and VT; the VT starts in text mode with an XLATE keyboard */
static void setup(void)
{
	assert(ev_eloop_new(&eloop) == 0);
	assert(ev_eloop_register_signal_cb(eloop, SIGTERM, sig_term, NULL) == 0);
	/*
	 * VT 0 is no VT to the libseat backend. pts 0, free now or freed by
	 * another process later, is kept open for the rest of the run.
	 */
	num = open_pty(&master, path, sizeof(path));
	while (num == 0)
		num = open_pty(&master, path, sizeof(path));
	fake_active = num + 1;
	fake_kbmode = K_XLATE;
	fake_kdmode = KD_TEXT;
	fake_vtmode = VT_AUTO;
	events[0] = 0;
	wait_error = 1;
}

static void teardown(void)
{
	ev_eloop_unregister_signal_cb(eloop, SIGTERM, sig_term, NULL);
	ev_eloop_unref(eloop);
	if (master >= 0)
		close(master);
}

/* the VT is as kmscon found it */
static bool vt_restored(void)
{
	return fake_kbmode == K_XLATE && fake_kdmode == KD_TEXT && fake_vtmode == VT_AUTO;
}

static void test_real_active_at_start(void)
{
	struct uterm_vt *vt;

	setup();
	fake_active = num;
	vt = uterm_vt_allocate(eloop, false, INPUT, path, &vt_cb, NULL);
	assert(vt);

	assert(uterm_vt_wait(vt, waited, NULL) == 0);
	assert(!strcmp(events, ""));
	ev_eloop_dispatch(eloop, 0);
	assert(!strcmp(events, "A"));

	/*
	 * a switch away and back: the wait that returned 0 left no callback
	 * behind for the next acquire to call, which would scan again
	 */
	kill(getpid(), SIGUSR2);
	ev_eloop_dispatch(eloop, 1000);
	kill(getpid(), SIGUSR1);
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, "ADA"));
	assert(uterm_vt_wait(vt, waited, NULL) == 0);

	uterm_vt_deallocate(vt);
	assert(!strcmp(events, "ADAD"));
	assert(vt_restored());
	teardown();
}

static void test_real_activated_later(void)
{
	struct uterm_vt *vt;

	setup();
	vt = uterm_vt_allocate(eloop, false, INPUT, path, &vt_cb, NULL);
	assert(vt);
	/* what a killed kmscon leaves on its VT */
	assert(fake_kbmode == K_OFF && fake_kdmode == KD_GRAPHICS && fake_vtmode == VT_PROCESS);

	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);
	assert(uterm_vt_wait(vt, waited, NULL) == -EALREADY);
	ev_eloop_dispatch(eloop, 0);
	assert(!strcmp(events, ""));

	/* an acquire signal while another VT is active does not end it */
	kill(getpid(), SIGUSR1);
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, ""));

	fake_active = num;
	kill(getpid(), SIGUSR1);
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, "WA") && wait_error == 0);

	assert(uterm_vt_wait(vt, waited, NULL) == 0);
	uterm_vt_deallocate(vt);
	assert(vt_restored());
	teardown();
}

static void test_real_sigterm_while_waiting(void)
{
	struct uterm_vt *vt;

	setup();
	vt = uterm_vt_allocate(eloop, false, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);

	raise(SIGTERM);
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, "T"));

	/* kmscon's exit frees the seat and its VT, dropping the wait */
	uterm_vt_deallocate(vt);
	assert(!strcmp(events, "T"));
	assert(vt_restored());
	teardown();
}

static void test_real_hup_while_waiting(void)
{
	struct uterm_vt *vt;

	setup();
	vt = uterm_vt_allocate(eloop, false, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);

	close(master);
	master = -1;
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, "WH") && wait_error == -EPIPE);
	assert(uterm_vt_wait(vt, waited, NULL) == -EPIPE);

	uterm_vt_deallocate(vt);
	teardown();
}

#ifdef BUILD_ENABLE_LIBSEAT

static int seat_opens;
static int active_at_open;  /* the VT active when the seat was opened */
static int seat_open_errno; /* opening the seat fails with this, if set */
static int seat_pipe[2];    /* the seat's fd, which never becomes readable */
static int seat_dummy;
static const struct libseat_seat_listener *seat_listener;
static void *seat_userdata;

#define SEAT ((struct libseat *)&seat_dummy)

struct libseat *libseat_open_seat(const struct libseat_seat_listener *listener, void *userdata)
{
	seat_opens++;
	active_at_open = fake_active;
	if (seat_open_errno) {
		errno = seat_open_errno;
		return NULL;
	}
	seat_listener = listener;
	seat_userdata = userdata;
	return SEAT;
}
int libseat_disable_seat(struct libseat *seat)
{
	return 0;
}
int libseat_close_seat(struct libseat *seat)
{
	return 0;
}
int libseat_open_device(struct libseat *seat, const char *path, int *fd)
{
	errno = ENODEV;
	return -1;
}
int libseat_close_device(struct libseat *seat, int device_id)
{
	return 0;
}
const char *libseat_seat_name(struct libseat *seat)
{
	return "seat0";
}
int libseat_switch_session(struct libseat *seat, int session)
{
	return 0;
}
int libseat_get_fd(struct libseat *seat)
{
	return seat_pipe[0];
}
int libseat_dispatch(struct libseat *seat, int timeout)
{
	return 0;
}
void libseat_set_log_handler(libseat_log_func handler) {}
void libseat_set_log_level(enum libseat_log_level level) {}

static void libseat_setup(void)
{
	setup();
	seat_opens = 0;
	active_at_open = -1;
	seat_open_errno = 0;
}

static void test_libseat_active_at_start(void)
{
	struct uterm_vt *vt;

	libseat_setup();
	fake_active = num;
	vt = uterm_vt_allocate(eloop, true, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(seat_opens == 0);

	assert(uterm_vt_wait(vt, waited, NULL) == 0);
	assert(seat_opens == 1 && active_at_open == num);

	uterm_vt_deallocate(vt);
	teardown();
}

static void test_libseat_activated_later(void)
{
	struct uterm_vt *vt;
	int turns;

	libseat_setup();
	vt = uterm_vt_allocate(eloop, true, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);

	/* polls that find the VT inactive, and a signal served between them */
	raise(SIGTERM);
	for (turns = 0; turns < 3; turns++)
		ev_eloop_dispatch(eloop, 300);
	assert(!strcmp(events, "T") && seat_opens == 0);

	fake_active = num;
	for (turns = 0; turns < 10 && !strchr(events, 'W'); turns++)
		ev_eloop_dispatch(eloop, 300);
	assert(!strcmp(events, "TW") && wait_error == 0);
	assert(seat_opens == 1 && active_at_open == num);

	/* the seat manager enables the seat: kmscon takes the VT's modes */
	seat_listener->enable_seat(SEAT, seat_userdata);
	assert(!strcmp(events, "TWA"));
	assert(fake_kbmode == K_OFF && fake_kdmode == KD_GRAPHICS);

	uterm_vt_deallocate(vt);
	assert(vt_restored());
	teardown();
}

static void test_libseat_open_fails(void)
{
	struct uterm_vt *vt;
	int turns;

	libseat_setup();
	seat_open_errno = EACCES;
	vt = uterm_vt_allocate(eloop, true, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);

	/* the VT becomes active, the seat does not open: the wait fails */
	fake_active = num;
	for (turns = 0; turns < 10 && !strchr(events, 'W'); turns++)
		ev_eloop_dispatch(eloop, 300);
	assert(!strcmp(events, "W") && wait_error == -EACCES);
	assert(seat_opens == 1);

	uterm_vt_deallocate(vt);
	assert(vt_restored());
	teardown();
}

static void test_libseat_exit_while_waiting(void)
{
	struct uterm_vt *vt;

	libseat_setup();
	vt = uterm_vt_allocate(eloop, true, INPUT, path, &vt_cb, NULL);
	assert(vt);
	assert(uterm_vt_wait(vt, waited, NULL) == -EINPROGRESS);

	raise(SIGTERM);
	ev_eloop_dispatch(eloop, 1000);
	assert(!strcmp(events, "T"));

	/* the poll goes with the VT: no wait ends, no seat opens */
	uterm_vt_deallocate(vt);
	fake_active = num;
	ev_eloop_dispatch(eloop, 300);
	assert(!strcmp(events, "T") && seat_opens == 0);
	assert(vt_restored());
	teardown();
}

#endif /* BUILD_ENABLE_LIBSEAT */

int main(void)
{
	test_real_active_at_start();
	test_real_activated_later();
	test_real_sigterm_while_waiting();
	test_real_hup_while_waiting();
#ifdef BUILD_ENABLE_LIBSEAT
	assert(pipe2(seat_pipe, O_CLOEXEC) == 0);
	test_libseat_active_at_start();
	test_libseat_activated_later();
	test_libseat_open_fails();
	test_libseat_exit_while_waiting();
	printf("test_vt_wait: ok (real, libseat)\n");
#else
	printf("test_vt_wait: ok (real; libseat not built)\n");
#endif
	return 0;
}
