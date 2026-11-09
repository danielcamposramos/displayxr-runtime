// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A legacy application's camera rig follows the depth limits (ORDER 478 milestone 2b).
 *
 * The instance holds what the rig is solved from: the person's choices (u_depth_settings, read again when they
 * change) and what the compositor announced (u_depth_limits_announced). On a locate, when any of them or the eyes or
 * the canvas changed, the rig's separation and zero-parallax plane are solved again (u_depth_rig_solve) and handed
 * back to be applied. The person's live keys still work on top of the values in force.
 */

#pragma once

#include "util/u_depth_limits.h"
#include "os/os_threading.h"
#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

struct oxr_depth_limits_state
{
	struct os_mutex lock;
	bool inited;
	bool driven; //!< the rig follows the limits (no per-title profile or override named numbers)
	struct u_depth_settings settings;
	uint64_t settings_serial;
	int64_t settings_checked_ns;
	bool applied;
	uint64_t applied_ann_serial;
	uint64_t applied_settings_serial;
	double applied_es, applied_aspect, applied_tan, applied_n;
	char last_refusal[1024]; //!< the refusal last logged, so a wrong value is said once, not every 500 ms
	bool said_none;
	bool said_none_for_serial_valid;
	uint64_t said_none_settings_serial;
};

//! The rig follows the limits from now on: read the person's choices and say what is in force.
void
oxr_depth_limits_start(struct oxr_depth_limits_state *st);

/*!
 * Solve the rig again if anything it depends on changed since the last call. True when @p out_ipd_factor and
 * @p out_inv_convergence are new values to apply; false when the rig stands as it is (nothing changed, or there are
 * no limits, which is said once).
 */
bool
oxr_depth_limits_update(struct oxr_depth_limits_state *st,
                        const struct xrt_vec3 *raw_eyes,
                        uint32_t eye_count,
                        float aspect,
                        float nominal_viewer_z,
                        float half_tan_vfov,
                        float m2v,
                        float *out_ipd_factor,
                        float *out_inv_convergence);

#ifdef __cplusplus
}
#endif
