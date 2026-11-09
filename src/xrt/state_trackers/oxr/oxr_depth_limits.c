// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A legacy application's camera rig follows the depth limits. See the header.
 */

#include "oxr_depth_limits.h"

#include "util/u_logging.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include <math.h>
#include <string.h>

#define SETTINGS_RECHECK_NS (500LL * 1000 * 1000)

static bool
same_settings(const struct u_depth_settings *a, const struct u_depth_settings *b)
{
	return a->cls == b->cls && a->strength_percent == b->strength_percent && a->plane_mm == b->plane_mm &&
	       a->sunk_px == b->sunk_px && a->popped_px == b->popped_px;
}

static void
reload_settings(struct oxr_depth_limits_state *st, bool first)
{
	struct u_depth_settings fresh;
	char message[1024] = {0};
	const bool ok = u_depth_settings_load(&fresh, message, sizeof(message));
	if (!ok) {
		if (strcmp(message, st->last_refusal) != 0) {
			U_LOG_E("%s", message);
			snprintf(st->last_refusal, sizeof(st->last_refusal), "%s", message);
		}
	} else {
		st->last_refusal[0] = '\0';
	}
	if (first || !same_settings(&fresh, &st->settings)) {
		st->settings = fresh;
		st->settings_serial++;
	}
}

void
oxr_depth_limits_start(struct oxr_depth_limits_state *st)
{
	if (!st->inited) {
		os_mutex_init(&st->lock);
		st->inited = true;
	}
	os_mutex_lock(&st->lock);
	st->driven = true;
	reload_settings(st, true);
	st->settings_checked_ns = (int64_t)os_monotonic_get_ns();
	os_mutex_unlock(&st->lock);
}

static const char *
source_text(const struct u_depth_resolved *r, const struct u_depth_settings *s, char *buf, size_t size)
{
	int n = snprintf(buf, size, "%s", r->source == U_DEPTH_SOURCE_COMPOSITOR ? "the compositor's, for this window's output"
	                                                                           : "the person's absolute values");
	if (r->source == U_DEPTH_SOURCE_COMPOSITOR) {
		if (s->cls != U_VIEW_CLASS_AUTO) {
			n += snprintf(buf + n, size - (size_t)n, "; class %s x%.3g", u_view_class_name(s->cls),
			              u_view_class_factor(s->cls));
		}
		if (s->strength_percent != 100.0) {
			n += snprintf(buf + n, size - (size_t)n, "; strength %.4g %%", s->strength_percent);
		}
		if (r->sunk_replaced) {
			n += snprintf(buf + n, size - (size_t)n, "; sunk replaced by the person's value");
		}
		if (r->popped_replaced) {
			snprintf(buf + n, size - (size_t)n, "; popped replaced by the person's value");
		}
	}
	return buf;
}

bool
oxr_depth_limits_update(struct oxr_depth_limits_state *st,
                        const struct xrt_vec3 *raw_eyes,
                        uint32_t eye_count,
                        float aspect,
                        float nominal_viewer_z,
                        float half_tan_vfov,
                        float m2v,
                        float *out_ipd_factor,
                        float *out_inv_convergence)
{
	if (!st->inited || !st->driven || eye_count < 2 || raw_eyes == NULL) {
		return false;
	}
	os_mutex_lock(&st->lock);
	const int64_t now = (int64_t)os_monotonic_get_ns();
	if (now - st->settings_checked_ns >= SETTINGS_RECHECK_NS) {
		st->settings_checked_ns = now;
		reload_settings(st, false);
	}

	struct u_depth_limits ann;
	uint64_t ann_serial = 0;
	const bool have_ann = u_depth_limits_announced(&ann, &ann_serial);
	struct u_depth_resolved r;
	if (!u_depth_resolve(&st->settings, have_ann ? &ann : NULL, &r)) {
		if (!st->said_none || st->said_none_settings_serial != st->settings_serial) {
			st->said_none = true;
			st->said_none_settings_serial = st->settings_serial;
			U_LOG_W("Depth limits: none. The compositor announced no depth limits for this window (Stereo KWin "
			        "sends them to a Wayland surface that asks, and writes _KDE_NET_WM_STEREO_DEPTH_LIMITS on an "
			        "X11 window that declares full side by side; neither has arrived), and no absolute sunk "
			        "value is set (DXR_VIEW_SUNK_PX, or sunk-px in the file view-depth). The camera rig keeps "
			        "DisplayXR's own defaults (ipdFactor 1, convergence 0.5 diopters); the class, strength and "
			        "screen plane have no limits to apply to.");
		}
		st->applied = false; // when limits come, the rig is solved again
		os_mutex_unlock(&st->lock);
		return false;
	}
	st->said_none = false;

	const double dx = (double)raw_eyes[1].x - raw_eyes[0].x;
	const double dy = (double)raw_eyes[1].y - raw_eyes[0].y;
	const double dz = (double)raw_eyes[1].z - raw_eyes[0].z;
	const double es = sqrt(dx * dx + dy * dy + dz * dz);
	if (!(es > 0.0) || !(aspect > 0.0)) {
		os_mutex_unlock(&st->lock);
		return false;
	}
	const double es_q = round(es * 1.0e4) / 1.0e4;
	const double aspect_q = round((double)aspect * 1.0e3) / 1.0e3;
	if (st->applied && st->applied_ann_serial == ann_serial && st->applied_settings_serial == st->settings_serial &&
	    st->applied_es == es_q && st->applied_aspect == aspect_q && st->applied_tan == (double)half_tan_vfov &&
	    st->applied_n == (double)nominal_viewer_z) {
		os_mutex_unlock(&st->lock);
		return false;
	}

	struct u_depth_rig_input in = {
	    .sunk_px = r.sunk_px,
	    .popped_px = r.popped_px,
	    .plane_m = r.plane_m,
	    .aspect = aspect_q,
	    .half_tan_vfov = half_tan_vfov,
	    .m2v = m2v,
	    .eye_separation = es_q,
	};
	struct u_depth_rig rig;
	if (!u_depth_rig_solve(&in, &rig)) {
		U_LOG_E("Depth limits: the rig cannot be solved (eye separation %.4f m, aspect %.3f, tan(vFov/2) %.4f, m2v "
		        "%.3g, plane %.3f m); it stays as it is.",
		        es_q, aspect_q, (double)half_tan_vfov, (double)m2v, r.plane_m);
		os_mutex_unlock(&st->lock);
		return false;
	}
	float ipd = rig.ipd_factor;
	float invc = rig.inv_convergence_distance;
	char notes[512] = {0};
	if (ipd < 0.01f) {
		ipd = 0.01f; // the rig's lowest separation: depth off is as flat as the rig goes
		snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes),
		         " The limits ask for less separation than the rig's lowest (0.01): that is used.");
	}
	// The runtime's comfort rule (dxr_rig_clamp_for_comfort): the zero-parallax plane no nearer than the viewer.
	if (nominal_viewer_z > 0.0f && m2v > 0.0f) {
		const float invc_max = 1.0f / ((ipd > 1.0f ? ipd : 1.0f) * m2v * nominal_viewer_z);
		if (invc > invc_max) {
			snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes),
			         " The comfort rule holds the plane at %.3f diopters (the viewer is %.2f m away), nearer than "
			         "the limit's %.3f: infinity then sinks less than the sunk limit.",
			         (double)invc_max, (double)nominal_viewer_z, (double)invc);
			invc = invc_max;
		}
	}
	if (r.source == U_DEPTH_SOURCE_PERSON && (st->settings.cls != U_VIEW_CLASS_AUTO || st->settings.strength_percent != 100.0)) {
		snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes),
		         " The class and the strength scale the compositor's limits; there are none, so the absolute values "
		         "are used as they are.");
	}
	char src[200];
	source_text(&r, &st->settings, src, sizeof(src));
	if (rig.nearest_m > 0.0) {
		U_LOG_W("Depth limits: sunk %.3f px, popped %.3f px of a view 1920 wide (%s); plane %.3f m: ipdFactor %.5g, "
		        "convergence %.5g diopters; infinity %.3f %% of the canvas behind the plane, the popped limit is "
		        "met at %.3f m and a scene nearer than that pops past it (the camera does what the geometry says).%s",
		        r.sunk_px, r.popped_px, src, r.plane_m, (double)ipd, (double)invc, rig.far_fraction * 100.0,
		        rig.nearest_m, notes);
	} else {
		U_LOG_W("Depth limits: sunk %.3f px of a view 1920 wide (%s), no popped limit known; plane %.3f m: ipdFactor "
		        "%.5g, convergence %.5g diopters; infinity %.3f %% of the canvas behind the plane.%s",
		        r.sunk_px, src, r.plane_m, (double)ipd, (double)invc, rig.far_fraction * 100.0, notes);
	}
	st->applied = true;
	st->applied_ann_serial = ann_serial;
	st->applied_settings_serial = st->settings_serial;
	st->applied_es = es_q;
	st->applied_aspect = aspect_q;
	st->applied_tan = (double)half_tan_vfov;
	st->applied_n = (double)nominal_viewer_z;
	os_mutex_unlock(&st->lock);
	*out_ipd_factor = ipd;
	*out_inv_convergence = invc;
	return true;
}
