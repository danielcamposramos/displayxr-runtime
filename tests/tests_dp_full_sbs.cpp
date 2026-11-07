// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The full side-by-side output flag and its back-compat gate.
 *
 * XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE makes the runtime present a target twice
 * the window's width and declare the window to a stereo desktop compositor. A
 * wrong "yes" doubles a window and declares a weave as stereo, so the reading
 * must be "no" in every case where the DP did not say yes: a plug-in built
 * before get_scanout_caps (whose slot bytes the runtime must never read), a
 * NULL slot, a declining DP, a DP that sets only the other flags.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_display_scanout.h"
#include "xrt/xrt_display_processor_vk.h"

#include <cstring>

namespace {

bool g_called = false;
bool g_answer = true;
uint32_t g_flags = 0;

bool
fake_get_scanout_caps(struct xrt_display_processor_vk *xdp, struct xrt_dp_scanout_caps *out_caps)
{
	(void)xdp;
	g_called = true;
	if (!g_answer) {
		return false;
	}
	out_caps->weave_scope = XRT_DP_WEAVE_SCOPE_CANVAS;
	out_caps->flags = g_flags;
	return true;
}

struct xrt_display_processor_vk
make_dp(uint32_t struct_size, bool with_fn)
{
	struct xrt_display_processor_vk dp;
	std::memset(&dp, 0, sizeof(dp));
	dp.base.struct_size = struct_size;
	dp.get_scanout_caps = with_fn ? fake_get_scanout_caps : nullptr;
	return dp;
}

//! `base.struct_size` of a plug-in built one slot before get_scanout_caps.
constexpr uint32_t
size_without_slot()
{
	return (uint32_t)offsetof(struct xrt_display_processor_vk, get_scanout_caps);
}

} // namespace

TEST_CASE("dp_full_sbs: the bit is new and distinct")
{
	CHECK(XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE == (1u << 2));
	CHECK((XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE & XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE) == 0);
	CHECK((XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE & XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN) == 0);
	// Carried in the existing flags word: no change to the caps layout.
	CHECK(sizeof(struct xrt_dp_scanout_caps) == XRT_DP_SCANOUT_CAPS_SIZE_V1);
}

TEST_CASE("dp_full_sbs: a pre-append plug-in is not full SBS and is never called")
{
	g_called = false;
	g_answer = true;
	g_flags = XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE;
	struct xrt_display_processor_vk dp = make_dp(size_without_slot(), /* with_fn */ true);

	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK_FALSE(g_called);
}

TEST_CASE("dp_full_sbs: NULL slot, a declining DP and no DP are not full SBS")
{
	g_called = false;
	g_answer = true;
	g_flags = XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE;
	struct xrt_display_processor_vk no_fn = make_dp(sizeof(no_fn), /* with_fn */ false);
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&no_fn));
	CHECK_FALSE(g_called);

	g_answer = false;
	struct xrt_display_processor_vk declining = make_dp(sizeof(declining), /* with_fn */ true);
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&declining));
	CHECK(g_called);

	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(nullptr));
}

TEST_CASE("dp_full_sbs: only the bit itself says full SBS")
{
	g_answer = true;
	struct xrt_display_processor_vk dp = make_dp(sizeof(dp), /* with_fn */ true);

	// A plug-in built before the bit wrote 0 here: an ordinary weave.
	g_flags = 0;
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));

	// Tolerating a resample (anaglyph, squeezed SBS) is not full SBS.
	g_flags = XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE | XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN;
	CHECK_FALSE(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK(xrt_display_processor_vk_tolerates_resample(&dp));

	g_flags = XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE | XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE;
	CHECK(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK(xrt_display_processor_vk_tolerates_resample(&dp));

	// The bit alone, too: full SBS does not imply resample tolerance.
	g_flags = XRT_DP_SCANOUT_FLAG_FULL_SIDE_BY_SIDE;
	CHECK(xrt_display_processor_vk_outputs_full_sbs(&dp));
	CHECK_FALSE(xrt_display_processor_vk_tolerates_resample(&dp));
}
