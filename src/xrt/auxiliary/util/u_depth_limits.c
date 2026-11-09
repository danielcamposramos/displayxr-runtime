// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The depth limits of a window's output and the camera rig that keeps to them. See the header.
 */

#include "util/u_depth_limits.h"

#include "xrt/xrt_config_os.h"

#include "os/os_threading.h"
#include "util/u_file.h"

#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * What the compositor announced.
 *
 */

static struct
{
	struct os_mutex lock;
	bool have;
	struct u_depth_limits limits;
	uint64_t serial;
} g_announced;
static atomic_int g_announced_state; // 0 not initialised, 1 initialising, 2 ready

static void
announced_lock(void)
{
	int expected = 0;
	if (atomic_compare_exchange_strong(&g_announced_state, &expected, 1)) {
		os_mutex_init(&g_announced.lock);
		atomic_store(&g_announced_state, 2);
	} else {
		while (atomic_load(&g_announced_state) != 2) {
		}
	}
	os_mutex_lock(&g_announced.lock);
}

static void
announced_unlock(void)
{
	os_mutex_unlock(&g_announced.lock);
}

void
u_depth_limits_publish(double sunk_px, double popped_px)
{
	announced_lock();
	if (!g_announced.have || g_announced.limits.sunk_px != sunk_px || g_announced.limits.popped_px != popped_px) {
		g_announced.have = true;
		g_announced.limits.sunk_px = sunk_px;
		g_announced.limits.popped_px = popped_px;
		g_announced.serial++;
	}
	announced_unlock();
}

void
u_depth_limits_withdraw(void)
{
	announced_lock();
	if (g_announced.have) {
		g_announced.have = false;
		g_announced.serial++;
	}
	announced_unlock();
}

bool
u_depth_limits_announced(struct u_depth_limits *out, uint64_t *out_serial)
{
	announced_lock();
	const bool have = g_announced.have;
	if (have && out != NULL) {
		*out = g_announced.limits;
	}
	if (out_serial != NULL) {
		*out_serial = g_announced.serial;
	}
	announced_unlock();
	return have;
}


/*
 *
 * The person's choice.
 *
 */

void
u_depth_settings_default(struct u_depth_settings *s)
{
	memset(s, 0, sizeof(*s));
	s->cls = U_VIEW_CLASS_AUTO;
	s->strength_percent = 100.0;
}

static void
say(char *message, size_t size, const char *fmt, const char *a, const char *b)
{
	if (message == NULL || size == 0) {
		return;
	}
	size_t used = strlen(message);
	if (used >= size) {
		return;
	}
	snprintf(message + used, size - used, fmt, a, b);
}

//! One number, all of it, finite and within [lo, hi].
static bool
parse_number(const char *text, double lo, double hi, double *out)
{
	if (text == NULL || text[0] == '\0') {
		return false;
	}
	char *end = NULL;
	errno = 0;
	const double v = strtod(text, &end);
	if (end == text || errno != 0 || !isfinite(v)) {
		return false;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0' || v < lo || v > hi) {
		return false;
	}
	*out = v;
	return true;
}

//! The value of @p key in `key=value` lines; false if the key is not there. @p value is a trimmed copy.
static bool
file_value(const char *text, const char *key, char *value, size_t value_size)
{
	if (text == NULL) {
		return false;
	}
	const size_t klen = strlen(key);
	const char *p = text;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		const size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
		if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			size_t n = len - klen - 1;
			if (n >= value_size) {
				n = value_size - 1;
			}
			memcpy(value, p + klen + 1, n);
			value[n] = '\0';
			while (n > 0 && (value[n - 1] == '\r' || value[n - 1] == ' ' || value[n - 1] == '\t')) {
				value[--n] = '\0';
			}
			return true;
		}
		p += len;
		if (*p == '\n') {
			p++;
		}
	}
	return false;
}

static bool
file_unknown_keys(const char *text, char *message, size_t message_size)
{
	static const char *const keys[] = {"depth-strength", "screen-plane-mm", "sunk-px", "popped-px"};
	if (text == NULL) {
		return true;
	}
	bool ok = true;
	const char *p = text;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		const size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
		if (len > 0 && p[0] != '#' && !(len == 1 && p[0] == '\r')) {
			bool known = false;
			for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
				const size_t kl = strlen(keys[i]);
				if (len > kl && strncmp(p, keys[i], kl) == 0 && p[kl] == '=') {
					known = true;
				}
			}
			if (!known) {
				char shown[48];
				snprintf(shown, sizeof(shown), "%.*s", (int)(len < 40 ? len : 40), p);
				say(message, message_size,
				    "depth: the file view-depth holds the line \"%s\", which is not a key (accepted: "
				    "depth-strength, screen-plane-mm, sunk-px, popped-px); ignored. %s",
				    shown, "");
				ok = false;
			}
		}
		p += len;
		if (*p == '\n') {
			p++;
		}
	}
	return ok;
}

//! One numeric setting: the environment first (a bad one is refused, never read past), then the file's key.
static bool
numeric(const char *env_name,
        const char *key,
        double lo,
        double hi,
        const char *(*env_get)(const char *, void *),
        void *user,
        const char *file_text,
        double *out,
        char *message,
        size_t message_size)
{
	char buf[64];
	const char *v = env_get != NULL ? env_get(env_name, user) : NULL;
	if (v != NULL && v[0] != '\0') {
		if (parse_number(v, lo, hi, out)) {
			return true;
		}
		char shown[48];
		snprintf(shown, sizeof(shown), "%.40s", v);
		say(message, message_size, "depth: %s holds \"%s\", which is not a number in range; not applied. ", env_name,
		    shown);
		return false;
	}
	if (file_value(file_text, key, buf, sizeof(buf))) {
		if (buf[0] == '\0') {
			return true; // an empty value selects nothing
		}
		if (parse_number(buf, lo, hi, out)) {
			return true;
		}
		char shown[48];
		snprintf(shown, sizeof(shown), "%.40s", buf);
		say(message, message_size,
		    "depth: the file view-depth holds %s for \"%s\", which is not a number in range; not applied. ", key,
		    shown);
		return false;
	}
	return true;
}

bool
u_depth_settings_parse(struct u_depth_settings *s,
                       const char *(*env_get)(const char *name, void *user),
                       void *user,
                       const char *view_depth_text,
                       const char *class_file_text,
                       char *message,
                       size_t message_size)
{
	u_depth_settings_default(s);
	if (message != NULL && message_size > 0) {
		message[0] = '\0';
	}
	bool ok = true;

	// the class: the environment (a bad value is refused and the file is not read past it), then the file
	const char *cv = env_get != NULL ? env_get("DXR_VIEW_CLASS", user) : NULL;
	if (cv != NULL && cv[0] != '\0') {
		if (!u_view_class_parse(cv, &s->cls)) {
			char shown[48];
			snprintf(shown, sizeof(shown), "%.40s", cv);
			say(message, message_size,
			    "depth: DXR_VIEW_CLASS holds \"%s\", which is not a display kind (accepted: " U_VIEW_CLASS_NAMES
			    "); not applied. %s",
			    shown, "");
			s->cls = U_VIEW_CLASS_AUTO;
			ok = false;
		}
	} else if (class_file_text != NULL) {
		char line[64];
		snprintf(line, sizeof(line), "%.60s", class_file_text);
		char *nl = strchr(line, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
			line[--n] = '\0';
		}
		if (line[0] != '\0' && !u_view_class_parse(line, &s->cls)) {
			say(message, message_size,
			    "depth: the file view-class holds \"%s\", which is not a display kind (accepted: " U_VIEW_CLASS_NAMES
			    "); not applied. %s",
			    line, "");
			s->cls = U_VIEW_CLASS_AUTO;
			ok = false;
		}
	}

	ok = file_unknown_keys(view_depth_text, message, message_size) && ok;
	ok = numeric("DXR_VIEW_DEPTH_STRENGTH", "depth-strength", 0.0, 1000.0, env_get, user, view_depth_text,
	             &s->strength_percent, message, message_size) && ok;
	ok = numeric("DXR_VIEW_SCREEN_PLANE_MM", "screen-plane-mm", 0.0, 1.0e6, env_get, user, view_depth_text,
	             &s->plane_mm, message, message_size) && ok;
	ok = numeric("DXR_VIEW_SUNK_PX", "sunk-px", 0.0, 7680.0, env_get, user, view_depth_text, &s->sunk_px, message,
	             message_size) && ok;
	ok = numeric("DXR_VIEW_POPPED_PX", "popped-px", 0.0, 7680.0, env_get, user, view_depth_text, &s->popped_px,
	             message, message_size) && ok;
	return ok;
}

static const char *
process_env(const char *name, void *user)
{
	(void)user;
	return getenv(name);
}

//! The text of a small file in the configuration directory; NULL when it is not there. Caller frees.
static char *
read_config_file(const char *name)
{
#ifndef XRT_OS_WINDOWS
	char path[1024];
	const int n = u_file_get_path_in_config_dir(name, path, sizeof(path));
	if (n <= 0 || (size_t)n >= sizeof(path)) {
		return NULL;
	}
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char *text = (char *)calloc(1, 4096);
	if (text != NULL) {
		const size_t got = fread(text, 1, 4095, f);
		text[got] = '\0';
	}
	fclose(f);
	return text;
#else
	(void)name;
	return NULL; // Windows: the environment only, until the machine-wide configuration path is decided
#endif
}

bool
u_depth_settings_load(struct u_depth_settings *s, char *message, size_t message_size)
{
	char *depth = read_config_file("view-depth");
	char *cls = read_config_file("view-class");
	const bool ok = u_depth_settings_parse(s, process_env, NULL, depth, cls, message, message_size);
	free(depth);
	free(cls);
	return ok;
}


/*
 *
 * Limits to the rig.
 *
 */

bool
u_depth_resolve(const struct u_depth_settings *s, const struct u_depth_limits *announced, struct u_depth_resolved *out)
{
	memset(out, 0, sizeof(*out));
	out->source = U_DEPTH_SOURCE_NONE;
	out->popped_px = -1.0;
	out->plane_m = (s->plane_mm > 0.0 ? s->plane_mm : U_DEPTH_LIMITS_DEFAULT_PLANE_MM) / 1000.0;
	if (announced == NULL && !(s->sunk_px > 0.0)) {
		return false;
	}
	if (announced != NULL) {
		const double factor = u_view_class_factor(s->cls) * s->strength_percent / 100.0;
		out->source = U_DEPTH_SOURCE_COMPOSITOR;
		out->sunk_px = announced->sunk_px * factor;
		out->popped_px = announced->popped_px >= 0.0 ? announced->popped_px * factor : -1.0;
	} else {
		out->source = U_DEPTH_SOURCE_PERSON;
	}
	if (s->sunk_px > 0.0) {
		out->sunk_px = s->sunk_px;
		out->sunk_replaced = true;
	}
	if (s->popped_px > 0.0) {
		out->popped_px = s->popped_px;
		out->popped_replaced = true;
	}
	return true;
}

bool
u_depth_rig_solve(const struct u_depth_rig_input *in, struct u_depth_rig *out)
{
	memset(out, 0, sizeof(*out));
	out->nearest_m = -1.0;
	out->popped_fraction = -1.0;
	if (!(in->aspect > 0.0) || !(in->half_tan_vfov > 0.0) || !(in->m2v > 0.0) || !(in->eye_separation > 0.0) ||
	    !(in->plane_m > 0.0) || !isfinite(in->sunk_px) || in->sunk_px < 0.0) {
		return false;
	}
	const double s = in->sunk_px / U_DEPTH_LIMITS_VIEW_WIDTH;
	// m2v cancels: the plane in world units is plane_m * m2v, and k carries m2v.
	out->ipd_factor = (float)(s * in->plane_m * 2.0 * in->half_tan_vfov * in->aspect / in->eye_separation);
	out->inv_convergence_distance = (float)(1.0 / (in->plane_m * in->m2v));
	out->far_fraction = s;
	if (in->popped_px >= 0.0) {
		const double q = in->popped_px / U_DEPTH_LIMITS_VIEW_WIDTH;
		out->popped_fraction = q;
		out->nearest_m = (s + q) > 0.0 ? in->plane_m * s / (s + q) : in->plane_m;
	}
	return true;
}

double
u_depth_rig_disparity(const struct u_depth_rig_input *in, const struct u_depth_rig *rig, double z_world)
{
	const double k = (double)rig->ipd_factor * in->m2v * in->eye_separation / (2.0 * in->half_tan_vfov * in->aspect);
	const double inv_z = z_world > 0.0 ? 1.0 / z_world : 0.0;
	return k * ((double)rig->inv_convergence_distance - inv_z);
}

double
u_depth_limits_plane_for_nearest(double sunk_px, double popped_px, double nearest_m)
{
	if (!(sunk_px > 0.0) || popped_px < 0.0 || !(nearest_m > 0.0)) {
		return -1.0;
	}
	return nearest_m * (sunk_px + popped_px) / sunk_px;
}
