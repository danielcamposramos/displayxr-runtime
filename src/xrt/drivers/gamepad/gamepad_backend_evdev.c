// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linux evdev backend for the gamepad provider.
 *
 * Reads the pads under /dev/input/event*: a node that has the BTN_GAMEPAD key (BTN_SOUTH), a left stick
 * (ABS_X, ABS_Y) and no relative axis is a pad by the kernel's gamepad protocol (Documentation/input/gamepad.rst).
 * Like XInput on Windows, evdev is focus-independent: the pad drives the rig whatever window is in front, which
 * is what a handle app (whose own window owns the keyboard) and a legacy app on a 3D screen with no head need.
 *
 * What the kernel decides is the access: a pad's node belongs to the session that owns the seat (udev's
 * uaccess ACL) or to the group `input`. A user with neither sees no pad, and the first scan says so.
 * Hot plug: inotify on /dev/input (a node created, removed, or given its ACL), and a slow rescan while no pad is
 * open. Several pads: all are read, the one touched last speaks. Rumble is not sent: the reader only reads.
 *
 * The logic is in gamepad_evdev_core.c (testable without a device) and gamepad_evdev_decode.c (pure); this file
 * is the system calls under it.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "gamepad_backend.h"
#include "gamepad_evdev_core.h"

#include "util/u_logging.h"
#include "os/os_time.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"

static struct gamepad_evdev_core g_core;
static int g_inotify = -1;
static bool g_up = false;
static bool g_rumble_said = false;
static char g_reason[160] = "evdev (not started)";

static int
sys_list(void *ctx, void (*emit)(void *user, const char *path), void *user)
{
	(void)ctx;
	DIR *dir = opendir(INPUT_DIR);
	if (dir == NULL) {
		return -1;
	}
	int n = 0;
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}
		char path[GAMEPAD_EVDEV_PATH_MAX];
		snprintf(path, sizeof(path), INPUT_DIR "/%s", e->d_name);
		emit(user, path);
		n++;
	}
	closedir(dir);
	return n;
}

static int
sys_open(void *ctx, const char *path)
{
	(void)ctx;
	const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	// The stamps of this node's events on the clock the reader compares them with.
	int clock_id = CLOCK_MONOTONIC;
	(void)ioctl(fd, EVIOCSCLOCKID, &clock_id);
	return fd;
}

#define BITS_LONG (8 * sizeof(unsigned long))
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_LONG] >> ((bit) % BITS_LONG)) & 1UL)

static const struct
{
	int code;
	int axis;
} k_axes[] = {
    {ABS_X, GP_EVDEV_AXIS_X},         {ABS_Y, GP_EVDEV_AXIS_Y},         {ABS_RX, GP_EVDEV_AXIS_RX},
    {ABS_RY, GP_EVDEV_AXIS_RY},       {ABS_Z, GP_EVDEV_AXIS_Z},         {ABS_RZ, GP_EVDEV_AXIS_RZ},
    {ABS_HAT0X, GP_EVDEV_AXIS_HAT0X}, {ABS_HAT0Y, GP_EVDEV_AXIS_HAT0Y}, {ABS_HAT1X, GP_EVDEV_AXIS_HAT1X},
    {ABS_HAT1Y, GP_EVDEV_AXIS_HAT1Y}, {ABS_HAT2X, GP_EVDEV_AXIS_HAT2X}, {ABS_HAT2Y, GP_EVDEV_AXIS_HAT2Y},
};

static enum gamepad_evdev_probe
sys_probe(void *ctx, int fd, struct gamepad_evdev_caps *caps, char *name, size_t name_size)
{
	(void)ctx;
	unsigned long ev_bits[(EV_MAX + BITS_LONG) / BITS_LONG];
	unsigned long key_bits[(KEY_MAX + BITS_LONG) / BITS_LONG];
	unsigned long abs_bits[(ABS_MAX + BITS_LONG) / BITS_LONG];
	memset(ev_bits, 0, sizeof(ev_bits));
	memset(key_bits, 0, sizeof(key_bits));
	memset(abs_bits, 0, sizeof(abs_bits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
		return GP_EVDEV_PROBE_ERROR;
	}
	if (!TEST_BIT(EV_KEY, ev_bits) || !TEST_BIT(EV_ABS, ev_bits)) {
		return GP_EVDEV_PROBE_NOT_A_PAD;
	}
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
		return GP_EVDEV_PROBE_ERROR;
	}
	memset(caps, 0, sizeof(*caps));
	for (size_t i = 0; i < sizeof(k_axes) / sizeof(k_axes[0]); i++) {
		if (!TEST_BIT(k_axes[i].code, abs_bits)) {
			continue;
		}
		struct input_absinfo info;
		if (ioctl(fd, EVIOCGABS(k_axes[i].code), &info) < 0) {
			return GP_EVDEV_PROBE_ERROR;
		}
		caps->axis[k_axes[i].axis].present = true;
		caps->axis[k_axes[i].axis].min = info.minimum;
		caps->axis[k_axes[i].axis].max = info.maximum;
	}
	if (!gamepad_evdev_is_gamepad(caps, TEST_BIT(BTN_SOUTH, key_bits) != 0, TEST_BIT(EV_REL, ev_bits) != 0)) {
		return GP_EVDEV_PROBE_NOT_A_PAD;
	}
	if (name_size > 0) {
		name[0] = '\0';
		if (ioctl(fd, EVIOCGNAME(name_size - 1), name) < 0) {
			name[0] = '\0';
		}
		name[name_size - 1] = '\0';
	}
	return GP_EVDEV_PROBE_PAD;
}

static ssize_t
sys_read(void *ctx, int fd, void *buf, size_t size)
{
	(void)ctx;
	return read(fd, buf, size);
}

static void
sys_resync(void *ctx, int fd, struct gamepad_evdev_decoder *d)
{
	(void)ctx;
	unsigned long key_state[(KEY_MAX + BITS_LONG) / BITS_LONG];
	memset(key_state, 0, sizeof(key_state));
	if (ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) >= 0) {
		static const uint16_t keys[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL,   BTN_TR,
		                                BTN_TL2,   BTN_TR2,  BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL,
		                                BTN_THUMBR, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
		for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			gamepad_evdev_decoder_set_key(d, keys[i], TEST_BIT(keys[i], key_state) != 0);
		}
	}
	for (size_t i = 0; i < sizeof(k_axes) / sizeof(k_axes[0]); i++) {
		if (!d->caps.axis[k_axes[i].axis].present) {
			continue;
		}
		struct input_absinfo info;
		if (ioctl(fd, EVIOCGABS(k_axes[i].code), &info) >= 0) {
			gamepad_evdev_decoder_set_abs(d, (uint16_t)k_axes[i].code, info.value);
		}
	}
}

static void
sys_close(void *ctx, int fd)
{
	(void)ctx;
	close(fd);
}

static bool
sys_changed(void *ctx)
{
	(void)ctx;
	if (g_inotify < 0) {
		return false;
	}
	bool any = false;
	char buf[4096];
	for (;;) {
		const ssize_t n = read(g_inotify, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		any = true;
	}
	return any;
}

static int64_t
sys_now(void *ctx)
{
	(void)ctx;
	return (int64_t)os_monotonic_get_ns();
}

static void
sys_say(void *ctx, int level, const char *line)
{
	(void)ctx;
	if (level == 0) {
		U_LOG_I("%s", line);
	} else {
		U_LOG_W("%s", line);
	}
}

static const struct gamepad_evdev_ops k_sys_ops = {
    .ctx = NULL,
    .list = sys_list,
    .open = sys_open,
    .probe = sys_probe,
    .read = sys_read,
    .resync = sys_resync,
    .close = sys_close,
    .changed = sys_changed,
    .now_ns = sys_now,
    .say = sys_say,
};

bool
gamepad_backend_available(void)
{
	// A reader exists wherever the input directory does: a pad plugged in later takes its roles without a
	// restart, as on Windows. Where it does not, the provider declines and says why (gamepad_backend_name).
	DIR *dir = opendir(INPUT_DIR);
	if (dir == NULL) {
		snprintf(g_reason, sizeof(g_reason), "evdev: " INPUT_DIR " cannot be opened (%s)", strerror(errno));
		return false;
	}
	closedir(dir);
	snprintf(g_reason, sizeof(g_reason), "evdev");
	return true;
}

bool
gamepad_backend_init(void)
{
	gamepad_evdev_core_init(&g_core, &k_sys_ops);
	g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (g_inotify >= 0 && inotify_add_watch(g_inotify, INPUT_DIR, IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
		U_LOG_W("gamepad: cannot watch " INPUT_DIR " for hot plug (%s); the pad list is read again every second "
		        "while none is attached.",
		        strerror(errno));
		close(g_inotify);
		g_inotify = -1;
	} else if (g_inotify < 0) {
		U_LOG_W("gamepad: inotify is not available (%s); the pad list is read again every second while none is "
		        "attached.",
		        strerror(errno));
	}
	g_up = true;
	return true;
}

void
gamepad_backend_fini(void)
{
	if (g_up) {
		gamepad_evdev_core_fini(&g_core);
	}
	if (g_inotify >= 0) {
		close(g_inotify);
		g_inotify = -1;
	}
	g_up = false;
}

void
gamepad_backend_poll(struct gamepad_state *out)
{
	if (!g_up) {
		memset(out, 0, sizeof(*out));
		return;
	}
	gamepad_evdev_core_poll(&g_core, out);
}

void
gamepad_backend_rumble(float low_frequency, float high_frequency)
{
	if ((low_frequency > 0.0f || high_frequency > 0.0f) && !g_rumble_said) {
		g_rumble_said = true;
		U_LOG_W("gamepad: rumble was asked for; the Linux reader only reads the pad and sends no force feedback.");
	}
}

const char *
gamepad_backend_name(void)
{
	return g_reason;
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_evdev_not_built_here_t;
#endif // XRT_OS_LINUX_DESKTOP
