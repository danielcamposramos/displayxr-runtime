// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The depth limits Stereo KWin tells the runtime's own X11 window in `_KDE_NET_WM_STEREO_DEPTH_LIMITS`.
 *
 * ORDER 485 (Sparky Stereo OS). Stereo KWin sends `kde_stereo_depth_v1.depth_limits` only to a Wayland surface that
 * asked for it, and the runtime's own window is an X11 client of Xwayland, which cannot. Stereo KWin therefore writes
 * the same two numbers on a window that declares stereo content, from declaration version 10 of
 * `_KDE_NET_WM_STEREO_CONTENT_SUPPORTED` on:
 *
 *   `_KDE_NET_WM_STEREO_DEPTH_LIMITS`, CARDINAL/32, exactly two values: the sunk limit and the popped limit, each a
 *   wl_fixed_t (24.8 fixed point, as the Wayland event carries it), in pixels of a view 1920 wide, for the output the
 *   window mainly sits on. The person's limits built on that display (a headset gets more separation, a framed 3D
 *   screen less). KWin keeps the property current and removes it when the window stops declaring.
 *
 * This is the reading of that value, with no X11 in it so it can be tested alone.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A window manager announcing this version of the stereo declaration writes the property.
#define COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MIN_SUPPORTED 10u

enum comp_vk_native_xcb_depth_limits_status
{
	COMP_VK_NATIVE_XCB_DEPTH_LIMITS_ABSENT = 0, //!< the window has no such property
	COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK,         //!< two limits were read
	COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED,  //!< a property that is not two non-negative CARDINAL/32 values
};

/*!
 * Read one reply of xcb_get_property for the property, asked for with a length of at least three 32-bit units
 * so that excess is seen. @p present is false when the property does not exist (type None).
 */
static inline enum comp_vk_native_xcb_depth_limits_status
comp_vk_native_xcb_depth_limits_decode(bool present,
                                       bool type_is_cardinal,
                                       uint8_t format,
                                       uint32_t value_len,
                                       uint32_t bytes_after,
                                       const uint32_t *values,
                                       double *out_sunk_px,
                                       double *out_popped_px)
{
	if (!present) {
		return COMP_VK_NATIVE_XCB_DEPTH_LIMITS_ABSENT;
	}
	if (!type_is_cardinal || format != 32 || value_len != 2 || bytes_after != 0 || values == NULL) {
		return COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED;
	}
	const int32_t sunk = (int32_t)values[0];
	const int32_t popped = (int32_t)values[1];
	if (sunk < 0 || popped < 0) {
		return COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED;
	}
	*out_sunk_px = (double)sunk / 256.0;
	*out_popped_px = (double)popped / 256.0;
	return COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK;
}

#ifdef __cplusplus
}
#endif
