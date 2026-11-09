// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The depth limits Stereo KWin writes on the runtime's own X11 window
 *         (comp_vk_native_xcb_depth_limits.h).
 *
 * `_KDE_NET_WM_STEREO_DEPTH_LIMITS` is CARDINAL/32 with exactly two values, the sunk and the popped limit as wl_fixed_t
 * (24.8 fixed point, what kde_stereo_depth_v1.depth_limits carries), in pixels of a view 1920 wide. Anything else is
 * said to be wrong, never read as limits.
 */

#include "catch_amalgamated.hpp"

#include "vk_native/comp_vk_native_xcb_depth_limits.h"

namespace {

enum comp_vk_native_xcb_depth_limits_status
decode(const uint32_t *values, uint32_t len, double *sunk, double *popped, bool present = true, bool cardinal = true,
       uint8_t format = 32, uint32_t bytes_after = 0)
{
	return comp_vk_native_xcb_depth_limits_decode(present, cardinal, format, len, bytes_after, values, sunk, popped);
}

} // namespace

TEST_CASE("depth limits: two 24.8 values are read as pixels", "[depth_limits][xcb]")
{
	double sunk = -1.0, popped = -1.0;
	const uint32_t a[2] = {10u * 256u, 4u * 256u};
	REQUIRE(decode(a, 2, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK);
	CHECK(sunk == 10.0);
	CHECK(popped == 4.0);
	// a fraction keeps its fraction: 9.5 px is 2432, 1.25 px is 320
	const uint32_t b[2] = {24u * 256u, 2432u};
	REQUIRE(decode(b, 2, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK);
	CHECK(sunk == 24.0);
	CHECK(popped == 9.5);
	const uint32_t c[2] = {768u, 320u};
	REQUIRE(decode(c, 2, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK);
	CHECK(sunk == 3.0);
	CHECK(popped == 1.25);
	// a limit of 0 is a limit (depth off), not an error
	const uint32_t z[2] = {0u, 0u};
	REQUIRE(decode(z, 2, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_OK);
	CHECK(sunk == 0.0);
	CHECK(popped == 0.0);
}

TEST_CASE("depth limits: a missing property is absent, not an error", "[depth_limits][xcb]")
{
	double sunk = -1.0, popped = -1.0;
	CHECK(decode(NULL, 0, &sunk, &popped, false, false, 0) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_ABSENT);
	CHECK(sunk == -1.0);
	CHECK(popped == -1.0);
}

TEST_CASE("depth limits: anything that is not two non-negative CARDINAL/32 values is malformed", "[depth_limits][xcb]")
{
	double sunk = -1.0, popped = -1.0;
	const uint32_t three[3] = {2560u, 1024u, 0u};
	const uint32_t two[2] = {2560u, 1024u};
	const uint32_t one[1] = {2560u};
	const uint32_t negative[2] = {0xFFFFFF00u, 1024u}; // sunk -1 px
	CHECK(decode(one, 1, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED);
	CHECK(decode(three, 3, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED);
	CHECK(decode(two, 2, &sunk, &popped, true, false) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED); // another type
	CHECK(decode(two, 2, &sunk, &popped, true, true, 16) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED);
	CHECK(decode(two, 2, &sunk, &popped, true, true, 32, 4) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED); // more follows
	CHECK(decode(negative, 2, &sunk, &popped) == COMP_VK_NATIVE_XCB_DEPTH_LIMITS_MALFORMED);
	CHECK(sunk == -1.0);
	CHECK(popped == -1.0);
}
