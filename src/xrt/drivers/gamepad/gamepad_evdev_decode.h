// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux pad decoder: evdev events in, @ref gamepad_state out. Pure.
 *
 * No file, no ioctl, no clock: the decoder is fed `struct input_event` arrays and keeps the pad's state, so
 * `tests/tests_gamepad_evdev.cpp` runs it on synthetic arrays. The device layer (`gamepad_evdev_core.c`) opens
 * the nodes and feeds it.
 *
 * The mapping follows the kernel's gamepad protocol (Documentation/input/gamepad.rst):
 *   - BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST = A, B, X, Y by physical position (the Xbox layout names of
 *     @ref gamepad_button; a Sony or Nintendo pad is mapped by position too);
 *   - BTN_TL, BTN_TR (or ABS_HAT1Y, ABS_HAT1X pressed) = the upper shoulder buttons;
 *   - the lower triggers are analog, 0..1: ABS_HAT2Y (left) and ABS_HAT2X (right) by the protocol, ABS_Z and
 *     ABS_RZ in the legacy drivers (xpad, hid-microsoft, hid-sony); a pad that has only BTN_TL2, BTN_TR2 gives
 *     0 or 1;
 *   - BTN_SELECT = View, BTN_START = Menu, BTN_MODE = Guide, BTN_THUMBL and BTN_THUMBR = the stick clicks;
 *   - the d-pad is BTN_DPAD_* or ABS_HAT0X and ABS_HAT0Y (negative is left and up), both read, either presses;
 *   - the left stick is ABS_X and ABS_Y, the right stick ABS_RX and ABS_RY (ABS_Z and ABS_RZ on the old
 *     pads that have no ABS_RX): for these axes "negative is left/up", so Y is negated to the +Y UP of
 *     @ref gamepad_state.
 * Each axis is normalised by its own range from EVIOCGABS: sticks to -1..1 about the middle of the range,
 * triggers to 0..1 from the minimum.
 *
 * A report is a batch ended by SYN_REPORT; the state changes only there, so a half-read batch is never seen.
 * SYN_DROPPED (the kernel's buffer overran) discards the batch and asks for a resync from the device's real
 * state (EVIOCGKEY, EVIOCGABS), which the device layer feeds back through the same setters.
 *
 * @ingroup drv_gamepad
 */

#pragma once

#include "gamepad_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct input_event;

//! The absolute axes the decoder reads.
enum gamepad_evdev_axis
{
	GP_EVDEV_AXIS_X = 0, //!< ABS_X, left stick
	GP_EVDEV_AXIS_Y,     //!< ABS_Y
	GP_EVDEV_AXIS_RX,    //!< ABS_RX, right stick
	GP_EVDEV_AXIS_RY,    //!< ABS_RY
	GP_EVDEV_AXIS_Z,     //!< ABS_Z, the legacy left trigger (or the right stick's X on a pad without ABS_RX)
	GP_EVDEV_AXIS_RZ,    //!< ABS_RZ, the legacy right trigger (or the right stick's Y)
	GP_EVDEV_AXIS_HAT0X, //!< the d-pad
	GP_EVDEV_AXIS_HAT0Y,
	GP_EVDEV_AXIS_HAT1X, //!< upper shoulders as analog buttons
	GP_EVDEV_AXIS_HAT1Y,
	GP_EVDEV_AXIS_HAT2X, //!< lower triggers
	GP_EVDEV_AXIS_HAT2Y,
	GP_EVDEV_AXIS_COUNT,
};

//! What the device reported about one axis (EVIOCGABS).
struct gamepad_evdev_axis_info
{
	bool present;
	int32_t min;
	int32_t max;
};

//! What the device is: the axes it has and their ranges.
struct gamepad_evdev_caps
{
	struct gamepad_evdev_axis_info axis[GP_EVDEV_AXIS_COUNT];
};

struct gamepad_evdev_decoder
{
	struct gamepad_evdev_caps caps;
	uint32_t key_buttons;             //!< @ref gamepad_button bits set by keys
	bool key_tl2, key_tr2;            //!< BTN_TL2, BTN_TR2 (digital lower triggers)
	int32_t raw[GP_EVDEV_AXIS_COUNT]; //!< latest raw value of each axis
	bool raw_seen[GP_EVDEV_AXIS_COUNT];
	bool dropped;                     //!< a SYN_DROPPED is being skipped to its SYN_REPORT
	bool needs_resync;                //!< the batch was lost: read the device's state and set it
	struct gamepad_state state;       //!< the last committed report
};

//! Start from the device's capabilities: every key up, every axis at the middle of its range.
void
gamepad_evdev_decoder_init(struct gamepad_evdev_decoder *d, const struct gamepad_evdev_caps *caps);

//! One key by its evdev code (EV_KEY): pressed or not. Codes the pad protocol does not use are ignored.
void
gamepad_evdev_decoder_set_key(struct gamepad_evdev_decoder *d, uint16_t code, bool down);

//! One axis by its evdev code (EV_ABS). Codes the decoder does not read are ignored.
void
gamepad_evdev_decoder_set_abs(struct gamepad_evdev_decoder *d, uint16_t code, int32_t value);

//! Compose the committed state from what was set (what SYN_REPORT does).
void
gamepad_evdev_decoder_commit(struct gamepad_evdev_decoder *d);

/*!
 * Feed @p count events. The state changes at each SYN_REPORT. Returns true when the state was committed at
 * least once. @ref gamepad_evdev_decoder::needs_resync tells the device layer that events were lost.
 */
bool
gamepad_evdev_decoder_feed(struct gamepad_evdev_decoder *d, const struct input_event *events, size_t count);

/*!
 * Is a device with these capabilities a gamepad by the kernel's protocol? It needs the BTN_GAMEPAD key
 * (BTN_SOUTH), a left stick (ABS_X, ABS_Y) and no relative axes. @p has_south and @p has_rel are the device's
 * key and event bits.
 */
bool
gamepad_evdev_is_gamepad(const struct gamepad_evdev_caps *caps, bool has_south, bool has_rel);

#ifdef __cplusplus
}
#endif
