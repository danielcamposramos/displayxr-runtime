// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux pad reader's device layer: which pads exist, which one speaks, and what happens when one goes.
 *
 * It owns no system call. Everything it needs from the system comes through @ref gamepad_evdev_ops, so
 * `tests/tests_gamepad_evdev.cpp` runs it on pipes and a scripted device list (hot plug, two pads, a pad that
 * goes away, a node that cannot be opened) and the real backend (`gamepad_backend_evdev.c`) hands it /dev/input.
 *
 * Rules:
 *   - every attached pad is read; the pad whose last report is newest speaks ("last used wins": a second pad
 *     lying on the desk does not hold the first);
 *   - a pad that fails to read (ENODEV, end of file) is closed and forgotten; the next pad takes over, and with
 *     none left the state is not connected;
 *   - the node list is read again when the system says it changed (@ref gamepad_evdev_ops::changed) and at
 *     most every @ref GAMEPAD_EVDEV_RESCAN_NS while a pad is missing, so a node whose permissions arrive a
 *     moment after it appears (udev's ACL) is tried again;
 *   - a node that cannot be opened is said once, by name and reason, not every scan; a node that is not a pad
 *     is not opened again until it goes.
 *
 * Called from the hub's poll thread only.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_evdev_decode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAMEPAD_EVDEV_MAX_PADS 8
#define GAMEPAD_EVDEV_PATH_MAX 96
#define GAMEPAD_EVDEV_RESCAN_NS (1000LL * 1000 * 1000)

//! What a node turned out to be.
enum gamepad_evdev_probe
{
	GP_EVDEV_PROBE_PAD = 0,
	GP_EVDEV_PROBE_NOT_A_PAD,
	GP_EVDEV_PROBE_ERROR,
};

struct gamepad_evdev_ops
{
	void *ctx;
	//! Call @p emit for each input node's path, in a stable order. -1 when the directory cannot be read.
	int (*list)(void *ctx, void (*emit)(void *user, const char *path), void *user);
	//! Open a node for reading without blocking. The descriptor, or -errno.
	int (*open)(void *ctx, const char *path);
	//! Is the open node a pad? Fills @p caps and @p name for a pad.
	enum gamepad_evdev_probe (*probe)(void *ctx, int fd, struct gamepad_evdev_caps *caps, char *name, size_t name_size);
	//! Read events like read(2): the byte count, 0 at end of file, -1 with errno (EAGAIN when none).
	ssize_t (*read)(void *ctx, int fd, void *buf, size_t size);
	//! Set the decoder from the device's own state after events were lost (EVIOCGKEY, EVIOCGABS).
	void (*resync)(void *ctx, int fd, struct gamepad_evdev_decoder *d);
	void (*close)(void *ctx, int fd);
	//! True once per change of the node list (inotify), false otherwise.
	bool (*changed)(void *ctx);
	//! Monotonic nanoseconds.
	int64_t (*now_ns)(void *ctx);
	//! One line for the log: level 0 info, 1 warning.
	void (*say)(void *ctx, int level, const char *line);
};

struct gamepad_evdev_pad
{
	bool used;
	int fd;
	char path[GAMEPAD_EVDEV_PATH_MAX];
	char name[64];
	struct gamepad_evdev_decoder dec;
	int64_t last_report_ns;
};

struct gamepad_evdev_core
{
	struct gamepad_evdev_ops ops;
	struct gamepad_evdev_pad pads[GAMEPAD_EVDEV_MAX_PADS];
	//! Paths seen and set aside (not a pad, or an error already said), until they leave the list.
	char skipped[32][GAMEPAD_EVDEV_PATH_MAX];
	int skipped_count;
	int64_t last_scan_ns;
	bool scanned_once;
	bool list_failed_said;
	bool denied_said;
};

void
gamepad_evdev_core_init(struct gamepad_evdev_core *c, const struct gamepad_evdev_ops *ops);

//! Read the pads and set @p out (every field; `connected` false with none). `timestamp_ns` is left to the caller.
void
gamepad_evdev_core_poll(struct gamepad_evdev_core *c, struct gamepad_state *out);

//! Close every pad.
void
gamepad_evdev_core_fini(struct gamepad_evdev_core *c);

//! How many pads are open.
int
gamepad_evdev_core_pad_count(const struct gamepad_evdev_core *c);

//! The name of the pad that speaks, or "" when none is open.
const char *
gamepad_evdev_core_speaking_name(const struct gamepad_evdev_core *c);

#ifdef __cplusplus
}
#endif
