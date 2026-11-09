// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux pad decoder. See the header.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "gamepad_evdev_decode.h"

#include <linux/input.h>

#include <string.h>

static int
axis_of_code(uint16_t code)
{
	switch (code) {
	case ABS_X: return GP_EVDEV_AXIS_X;
	case ABS_Y: return GP_EVDEV_AXIS_Y;
	case ABS_RX: return GP_EVDEV_AXIS_RX;
	case ABS_RY: return GP_EVDEV_AXIS_RY;
	case ABS_Z: return GP_EVDEV_AXIS_Z;
	case ABS_RZ: return GP_EVDEV_AXIS_RZ;
	case ABS_HAT0X: return GP_EVDEV_AXIS_HAT0X;
	case ABS_HAT0Y: return GP_EVDEV_AXIS_HAT0Y;
	case ABS_HAT1X: return GP_EVDEV_AXIS_HAT1X;
	case ABS_HAT1Y: return GP_EVDEV_AXIS_HAT1Y;
	case ABS_HAT2X: return GP_EVDEV_AXIS_HAT2X;
	case ABS_HAT2Y: return GP_EVDEV_AXIS_HAT2Y;
	default: return -1;
	}
}

static uint32_t
button_of_code(uint16_t code)
{
	switch (code) {
	case BTN_SOUTH: return GAMEPAD_BTN_A;
	case BTN_EAST: return GAMEPAD_BTN_B;
	case BTN_NORTH: return GAMEPAD_BTN_X;
	case BTN_WEST: return GAMEPAD_BTN_Y;
	case BTN_TL: return GAMEPAD_BTN_SHOULDER_LEFT;
	case BTN_TR: return GAMEPAD_BTN_SHOULDER_RIGHT;
	case BTN_THUMBL: return GAMEPAD_BTN_THUMB_LEFT;
	case BTN_THUMBR: return GAMEPAD_BTN_THUMB_RIGHT;
	case BTN_START: return GAMEPAD_BTN_MENU;
	case BTN_SELECT: return GAMEPAD_BTN_VIEW;
	case BTN_MODE: return GAMEPAD_BTN_GUIDE;
	case BTN_DPAD_UP: return GAMEPAD_BTN_DPAD_UP;
	case BTN_DPAD_DOWN: return GAMEPAD_BTN_DPAD_DOWN;
	case BTN_DPAD_LEFT: return GAMEPAD_BTN_DPAD_LEFT;
	case BTN_DPAD_RIGHT: return GAMEPAD_BTN_DPAD_RIGHT;
	default: return 0;
	}
}

static int32_t
rest_value(const struct gamepad_evdev_axis_info *a, int axis)
{
	if (!a->present) {
		return 0;
	}
	switch (axis) {
	case GP_EVDEV_AXIS_X:
	case GP_EVDEV_AXIS_Y:
	case GP_EVDEV_AXIS_RX:
	case GP_EVDEV_AXIS_RY: return a->min + (a->max - a->min) / 2;
	case GP_EVDEV_AXIS_HAT0X:
	case GP_EVDEV_AXIS_HAT0Y: return 0;
	default: return a->min; // triggers and upper shoulders rest at their minimum
	}
}

void
gamepad_evdev_decoder_init(struct gamepad_evdev_decoder *d, const struct gamepad_evdev_caps *caps)
{
	memset(d, 0, sizeof(*d));
	d->caps = *caps;
	for (int i = 0; i < GP_EVDEV_AXIS_COUNT; i++) {
		d->raw[i] = rest_value(&d->caps.axis[i], i);
	}
	d->state.connected = true;
}

void
gamepad_evdev_decoder_set_key(struct gamepad_evdev_decoder *d, uint16_t code, bool down)
{
	if (code == BTN_TL2) {
		d->key_tl2 = down;
		return;
	}
	if (code == BTN_TR2) {
		d->key_tr2 = down;
		return;
	}
	const uint32_t bit = button_of_code(code);
	if (bit == 0) {
		return;
	}
	if (down) {
		d->key_buttons |= bit;
	} else {
		d->key_buttons &= ~bit;
	}
}

void
gamepad_evdev_decoder_set_abs(struct gamepad_evdev_decoder *d, uint16_t code, int32_t value)
{
	const int axis = axis_of_code(code);
	if (axis < 0 || !d->caps.axis[axis].present) {
		return;
	}
	d->raw[axis] = value;
	d->raw_seen[axis] = true;
}

//! -1..1 about the middle of the axis' range.
static float
signed_unit(const struct gamepad_evdev_axis_info *a, int32_t v)
{
	if (!a->present || a->max <= a->min) {
		return 0.0f;
	}
	if (v < a->min) {
		v = a->min;
	}
	if (v > a->max) {
		v = a->max;
	}
	const double n = 2.0 * (double)((int64_t)v - a->min) / (double)((int64_t)a->max - a->min) - 1.0;
	return (float)n;
}

//! 0..1 from the minimum of the axis' range.
static float
positive_unit(const struct gamepad_evdev_axis_info *a, int32_t v)
{
	if (!a->present || a->max <= a->min) {
		return 0.0f;
	}
	if (v < a->min) {
		v = a->min;
	}
	if (v > a->max) {
		v = a->max;
	}
	return (float)((double)((int64_t)v - a->min) / (double)((int64_t)a->max - a->min));
}

static float
max2(float a, float b)
{
	return a > b ? a : b;
}

void
gamepad_evdev_decoder_commit(struct gamepad_evdev_decoder *d)
{
	const struct gamepad_evdev_caps *c = &d->caps;
	struct gamepad_state s = d->state;
	s.connected = true;
	s.buttons = d->key_buttons;

	// The d-pad as an analog hat: negative is left and up.
	const struct gamepad_evdev_axis_info *hx = &c->axis[GP_EVDEV_AXIS_HAT0X];
	const struct gamepad_evdev_axis_info *hy = &c->axis[GP_EVDEV_AXIS_HAT0Y];
	if (hx->present) {
		if (d->raw[GP_EVDEV_AXIS_HAT0X] < 0) {
			s.buttons |= GAMEPAD_BTN_DPAD_LEFT;
		} else if (d->raw[GP_EVDEV_AXIS_HAT0X] > 0) {
			s.buttons |= GAMEPAD_BTN_DPAD_RIGHT;
		}
	}
	if (hy->present) {
		if (d->raw[GP_EVDEV_AXIS_HAT0Y] < 0) {
			s.buttons |= GAMEPAD_BTN_DPAD_UP;
		} else if (d->raw[GP_EVDEV_AXIS_HAT0Y] > 0) {
			s.buttons |= GAMEPAD_BTN_DPAD_DOWN;
		}
	}

	// Upper shoulders reported as analog buttons: pressed past the middle of the range.
	const struct gamepad_evdev_axis_info *u_r = &c->axis[GP_EVDEV_AXIS_HAT1X];
	const struct gamepad_evdev_axis_info *u_l = &c->axis[GP_EVDEV_AXIS_HAT1Y];
	if (u_r->present && positive_unit(u_r, d->raw[GP_EVDEV_AXIS_HAT1X]) > 0.5f) {
		s.buttons |= GAMEPAD_BTN_SHOULDER_RIGHT;
	}
	if (u_l->present && positive_unit(u_l, d->raw[GP_EVDEV_AXIS_HAT1Y]) > 0.5f) {
		s.buttons |= GAMEPAD_BTN_SHOULDER_LEFT;
	}

	// Sticks. The kernel's "negative is left/up" becomes gamepad_state's +X right, +Y up.
	s.left_x = signed_unit(&c->axis[GP_EVDEV_AXIS_X], d->raw[GP_EVDEV_AXIS_X]);
	s.left_y = -signed_unit(&c->axis[GP_EVDEV_AXIS_Y], d->raw[GP_EVDEV_AXIS_Y]);
	const bool has_rx = c->axis[GP_EVDEV_AXIS_RX].present && c->axis[GP_EVDEV_AXIS_RY].present;
	const bool z_is_stick = !has_rx && c->axis[GP_EVDEV_AXIS_Z].present && c->axis[GP_EVDEV_AXIS_RZ].present;
	if (has_rx) {
		s.right_x = signed_unit(&c->axis[GP_EVDEV_AXIS_RX], d->raw[GP_EVDEV_AXIS_RX]);
		s.right_y = -signed_unit(&c->axis[GP_EVDEV_AXIS_RY], d->raw[GP_EVDEV_AXIS_RY]);
	} else if (z_is_stick) {
		s.right_x = signed_unit(&c->axis[GP_EVDEV_AXIS_Z], d->raw[GP_EVDEV_AXIS_Z]);
		s.right_y = -signed_unit(&c->axis[GP_EVDEV_AXIS_RZ], d->raw[GP_EVDEV_AXIS_RZ]);
	} else {
		s.right_x = 0.0f;
		s.right_y = 0.0f;
	}

	// Lower triggers: the protocol's ABS_HAT2*, the legacy drivers' ABS_Z / ABS_RZ, else the digital keys.
	float lt = positive_unit(&c->axis[GP_EVDEV_AXIS_HAT2Y], d->raw[GP_EVDEV_AXIS_HAT2Y]);
	float rt = positive_unit(&c->axis[GP_EVDEV_AXIS_HAT2X], d->raw[GP_EVDEV_AXIS_HAT2X]);
	if (!z_is_stick) {
		lt = max2(lt, positive_unit(&c->axis[GP_EVDEV_AXIS_Z], d->raw[GP_EVDEV_AXIS_Z]));
		rt = max2(rt, positive_unit(&c->axis[GP_EVDEV_AXIS_RZ], d->raw[GP_EVDEV_AXIS_RZ]));
	}
	if (d->key_tl2) {
		lt = max2(lt, 1.0f);
	}
	if (d->key_tr2) {
		rt = max2(rt, 1.0f);
	}
	s.left_trigger = lt;
	s.right_trigger = rt;
	d->state = s;
}

bool
gamepad_evdev_decoder_feed(struct gamepad_evdev_decoder *d, const struct input_event *events, size_t count)
{
	bool committed = false;
	for (size_t i = 0; i < count; i++) {
		const struct input_event *e = &events[i];
		if (e->type == EV_SYN) {
			if (e->code == SYN_DROPPED) {
				// The kernel's buffer overran: the rest of this batch is not to be trusted.
				d->dropped = true;
				d->needs_resync = true;
			} else if (e->code == SYN_REPORT) {
				if (d->dropped) {
					d->dropped = false; // the lost batch ends here; the resync fills the state in
				} else {
					gamepad_evdev_decoder_commit(d);
					committed = true;
				}
			}
			continue;
		}
		if (d->dropped) {
			continue;
		}
		if (e->type == EV_KEY) {
			// value 2 is autorepeat: still down
			gamepad_evdev_decoder_set_key(d, e->code, e->value != 0);
		} else if (e->type == EV_ABS) {
			gamepad_evdev_decoder_set_abs(d, e->code, e->value);
		}
	}
	return committed;
}

bool
gamepad_evdev_is_gamepad(const struct gamepad_evdev_caps *caps, bool has_south, bool has_rel)
{
	return has_south && !has_rel && caps->axis[GP_EVDEV_AXIS_X].present && caps->axis[GP_EVDEV_AXIS_Y].present;
}

#else
// Keep the translation unit non-empty where this decoder is compiled out (-Wempty-translation-unit).
typedef int gamepad_evdev_decode_not_built_here_t;
#endif // XRT_OS_LINUX_DESKTOP
