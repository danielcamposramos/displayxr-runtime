// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux pad reader's device layer. See the header.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "gamepad_evdev_core.h"

#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#define CORE_BATCH 64

static void
say(struct gamepad_evdev_core *c, int level, const char *fmt, const char *a, const char *b)
{
	char line[256];
	snprintf(line, sizeof(line), fmt, a, b);
	c->ops.say(c->ops.ctx, level, line);
}

static bool
skipped_has(const struct gamepad_evdev_core *c, const char *path)
{
	for (int i = 0; i < c->skipped_count; i++) {
		if (strcmp(c->skipped[i], path) == 0) {
			return true;
		}
	}
	return false;
}

static void
skipped_add(struct gamepad_evdev_core *c, const char *path)
{
	if (c->skipped_count < (int)(sizeof(c->skipped) / sizeof(c->skipped[0])) && !skipped_has(c, path)) {
		snprintf(c->skipped[c->skipped_count++], GAMEPAD_EVDEV_PATH_MAX, "%s", path);
	}
}

static struct gamepad_evdev_pad *
pad_by_path(struct gamepad_evdev_core *c, const char *path)
{
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		if (c->pads[i].used && strcmp(c->pads[i].path, path) == 0) {
			return &c->pads[i];
		}
	}
	return NULL;
}

static void
drop_pad(struct gamepad_evdev_core *c, struct gamepad_evdev_pad *p, const char *why)
{
	say(c, 1, "gamepad: pad %s is gone: %s.", p->name, why);
	c->ops.close(c->ops.ctx, p->fd);
	memset(p, 0, sizeof(*p));
}

struct scan_state
{
	struct gamepad_evdev_core *core;
	// the paths the list emitted this time, to forget what left
	char seen[64][GAMEPAD_EVDEV_PATH_MAX];
	int seen_count;
	int opened;
	int denied;
};

static void
consider(void *user, const char *path)
{
	struct scan_state *st = (struct scan_state *)user;
	struct gamepad_evdev_core *c = st->core;
	if (st->seen_count < (int)(sizeof(st->seen) / sizeof(st->seen[0]))) {
		snprintf(st->seen[st->seen_count++], GAMEPAD_EVDEV_PATH_MAX, "%s", path);
	}
	if (pad_by_path(c, path) != NULL || skipped_has(c, path)) {
		return;
	}
	struct gamepad_evdev_pad *slot = NULL;
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		if (!c->pads[i].used) {
			slot = &c->pads[i];
			break;
		}
	}
	if (slot == NULL) {
		return; // eight pads are read; a ninth waits until one goes
	}
	const int fd = c->ops.open(c->ops.ctx, path);
	if (fd < 0) {
		// A node the session may not read is normal (keyboards, mice) and is tried again at the next scan: udev
		// gives a pad's node its ACL a moment after the node appears. The scan says it once when nothing at all
		// could be read.
		if (-fd == EACCES || -fd == EPERM) {
			st->denied++;
			return;
		}
		char reason[64];
		snprintf(reason, sizeof(reason), "error %d", -fd);
		say(c, 1, "gamepad: cannot open %s: %s.", path, reason);
		skipped_add(c, path);
		return;
	}
	st->opened++;
	struct gamepad_evdev_caps caps;
	char name[64] = {0};
	const enum gamepad_evdev_probe pr = c->ops.probe(c->ops.ctx, fd, &caps, name, sizeof(name));
	if (pr != GP_EVDEV_PROBE_PAD) {
		c->ops.close(c->ops.ctx, fd);
		if (pr == GP_EVDEV_PROBE_ERROR) {
			say(c, 1, "gamepad: cannot read the capabilities of %s%s.", path, "");
		}
		skipped_add(c, path);
		return;
	}
	memset(slot, 0, sizeof(*slot));
	slot->used = true;
	slot->fd = fd;
	snprintf(slot->path, sizeof(slot->path), "%s", path);
	snprintf(slot->name, sizeof(slot->name), "%s", name[0] ? name : "pad");
	gamepad_evdev_decoder_init(&slot->dec, &caps);
	// A pad that was already held when it was found starts from its real state.
	c->ops.resync(c->ops.ctx, fd, &slot->dec);
	gamepad_evdev_decoder_commit(&slot->dec);
	slot->last_report_ns = c->ops.now_ns(c->ops.ctx);
	say(c, 0, "gamepad: pad attached: %s at %s.", slot->name, path);
}

static void
scan(struct gamepad_evdev_core *c)
{
	struct scan_state st;
	memset(&st, 0, sizeof(st));
	st.core = c;
	const int n = c->ops.list(c->ops.ctx, consider, &st);
	c->last_scan_ns = c->ops.now_ns(c->ops.ctx);
	c->scanned_once = true;
	if (n < 0) {
		if (!c->list_failed_said) {
			c->list_failed_said = true;
			say(c, 1, "gamepad: the input directory cannot be read; no pad is available.", "", "");
		}
		return;
	}
	c->list_failed_said = false;
	if (st.denied > 0 && st.opened == 0 && gamepad_evdev_core_pad_count(c) == 0) {
		if (!c->denied_said) {
			c->denied_said = true;
			say(c, 1,
			    "gamepad: no input node can be read by this user (the pad's node is readable by the session "
			    "that owns the seat, or by the group input); no pad is available until one can.",
			    "", "");
		}
	} else {
		c->denied_said = false;
	}
	// forget what left the list, so a node that returns is looked at again
	for (int i = 0; i < c->skipped_count;) {
		bool still = false;
		for (int j = 0; j < st.seen_count; j++) {
			if (strcmp(c->skipped[i], st.seen[j]) == 0) {
				still = true;
				break;
			}
		}
		if (still) {
			i++;
		} else {
			memmove(c->skipped[i], c->skipped[c->skipped_count - 1], GAMEPAD_EVDEV_PATH_MAX);
			c->skipped_count--;
		}
	}
	// a pad whose node left the list is gone even if its descriptor still reads
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		struct gamepad_evdev_pad *p = &c->pads[i];
		if (!p->used) {
			continue;
		}
		bool listed = false;
		for (int j = 0; j < st.seen_count; j++) {
			if (strcmp(p->path, st.seen[j]) == 0) {
				listed = true;
				break;
			}
		}
		if (!listed) {
			drop_pad(c, p, "its node left /dev/input");
		}
	}
}

void
gamepad_evdev_core_init(struct gamepad_evdev_core *c, const struct gamepad_evdev_ops *ops)
{
	memset(c, 0, sizeof(*c));
	c->ops = *ops;
}

// When the batch's last report happened, by the kernel's own stamp: two pads read in one poll keep the order in which
// they were touched, not the order in which they are read. A report with no stamp takes the time of the read.
static int64_t
last_report_time(const struct input_event *ev, size_t count, int64_t read_time)
{
	for (size_t i = count; i-- > 0;) {
		if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
			const int64_t ns = (int64_t)ev[i].input_event_sec * 1000000000LL + (int64_t)ev[i].input_event_usec * 1000LL;
			return ns != 0 ? ns : read_time;
		}
	}
	return read_time;
}

static void
read_pad(struct gamepad_evdev_core *c, struct gamepad_evdev_pad *p)
{
	struct input_event ev[CORE_BATCH];
	for (;;) {
		const ssize_t n = c->ops.read(c->ops.ctx, p->fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				return;
			}
			drop_pad(c, p, errno == ENODEV ? "unplugged" : "read failed");
			return;
		}
		if (n == 0) {
			drop_pad(c, p, "end of file");
			return;
		}
		const size_t count = (size_t)n / sizeof(struct input_event);
		if (gamepad_evdev_decoder_feed(&p->dec, ev, count)) {
			p->last_report_ns = last_report_time(ev, count, c->ops.now_ns(c->ops.ctx));
		}
		if (p->dec.needs_resync) {
			// The kernel dropped events: take the device's own state, not the lost batch's.
			c->ops.resync(c->ops.ctx, p->fd, &p->dec);
			p->dec.needs_resync = false;
			gamepad_evdev_decoder_commit(&p->dec);
			p->last_report_ns = c->ops.now_ns(c->ops.ctx);
		}
		if ((size_t)n < sizeof(ev)) {
			return;
		}
	}
}

void
gamepad_evdev_core_poll(struct gamepad_evdev_core *c, struct gamepad_state *out)
{
	const int64_t now = c->ops.now_ns(c->ops.ctx);
	const bool changed = c->ops.changed(c->ops.ctx);
	const bool missing = gamepad_evdev_core_pad_count(c) == 0;
	if (!c->scanned_once || changed || (missing && now - c->last_scan_ns >= GAMEPAD_EVDEV_RESCAN_NS)) {
		scan(c);
	}

	struct gamepad_evdev_pad *best = NULL;
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		struct gamepad_evdev_pad *p = &c->pads[i];
		if (!p->used) {
			continue;
		}
		read_pad(c, p);
		if (p->used && (best == NULL || p->last_report_ns > best->last_report_ns)) {
			best = p;
		}
	}
	if (best == NULL) {
		memset(out, 0, sizeof(*out));
		out->connected = false;
		return;
	}
	*out = best->dec.state;
	out->connected = true;
}

void
gamepad_evdev_core_fini(struct gamepad_evdev_core *c)
{
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		if (c->pads[i].used) {
			c->ops.close(c->ops.ctx, c->pads[i].fd);
			memset(&c->pads[i], 0, sizeof(c->pads[i]));
		}
	}
}

int
gamepad_evdev_core_pad_count(const struct gamepad_evdev_core *c)
{
	int n = 0;
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		n += c->pads[i].used ? 1 : 0;
	}
	return n;
}

const char *
gamepad_evdev_core_speaking_name(const struct gamepad_evdev_core *c)
{
	const struct gamepad_evdev_pad *best = NULL;
	for (int i = 0; i < GAMEPAD_EVDEV_MAX_PADS; i++) {
		const struct gamepad_evdev_pad *p = &c->pads[i];
		if (p->used && (best == NULL || p->last_report_ns > best->last_report_ns)) {
			best = p;
		}
	}
	return best != NULL ? best->name : "";
}

#else
// Keep the translation unit non-empty where this layer is compiled out (-Wempty-translation-unit).
typedef int gamepad_evdev_core_not_built_here_t;
#endif // XRT_OS_LINUX_DESKTOP
