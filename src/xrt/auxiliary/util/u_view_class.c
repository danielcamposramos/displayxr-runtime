// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The person's choice of display kind for legacy applications' views, and the camera-rig defaults per kind.
 *
 * ORDER 478 milestone 2 (Sparky Stereo OS). The two kinds follow the edition's one depth source, libstereo-policy
 * (stereo_policy_display_depth, which the desktop's windows and the browser's pages already use):
 *   - a headset takes 2.5 times the separation of a framed screen (STEREO_POLICY_DISPLAY_HEADSET 2.5);
 *   - the popped limit is 0.4 of the sunk limit (0.079 and 0.198 degrees at the eyes).
 * Two numbers of the closed form in docs/specs/runtime/legacy-camera-profile.md ("Disparity does not depend on the
 * display") then fix the pair, as a fraction of the canvas width (positive behind the screen plane):
 *
 *   p(Z)/W = k (c - 1/Z),   k = ipd * m2v * eyeSeparation / (2 tan(vFov/2) aspect),   c = convergence in diopters
 *
 *   - screen: a point at infinity sinks 2.0 % of the width (the edition's browser, its flat TV mode, measured), and
 *     the nearest working distance, 0.8 m, pops 0.4 of that, 0.8 %. With the display's reported eye separation of
 *     60 mm (sim_display), tan(vFov/2) 0.3249 and a 16:9 canvas that is ipd 0.43 and c 0.89 (the plane at 1.12 m).
 *   - headset: the same plane, 2.5 times the separation, ipd 1.075: infinity 5.0 %, 0.8 m pops 2.0 %.
 * The 2.0 %, the 0.8 m and the plane they give are proposals: no design of Daniel's names them. His call.
 */

#include "util/u_view_class.h"

#include "xrt/xrt_config_os.h"

#include "util/u_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_CLASS_FILE "view-class"
#define VIEW_CLASS_ENV "DXR_VIEW_CLASS"

static const char *const class_names[] = {"screen", "headset"};

bool
u_view_class_parse(const char *name, enum u_view_class *out)
{
	if (name == NULL)
		return false;
	for (size_t i = 0; i < sizeof(class_names) / sizeof(class_names[0]); i++) {
		if (strcmp(name, class_names[i]) == 0) {
			*out = (enum u_view_class)i;
			return true;
		}
	}
	return false;
}

const char *
u_view_class_name(enum u_view_class cls)
{
	return class_names[cls == U_VIEW_CLASS_HEADSET ? 1 : 0];
}

void
u_view_class_profile(enum u_view_class cls, struct u_camera_profile *out)
{
	struct u_camera_profile p = U_CAMERA_PROFILE_DEFAULT;
	p.ipd_factor = cls == U_VIEW_CLASS_HEADSET ? 1.075f : 0.43f;
	p.inv_convergence_distance = 0.89f;
	*out = p;
}

static void
say(char *message, size_t size, const char *source, const char *value)
{
	if (message == NULL || size == 0)
		return;
	snprintf(message, size,
	         "view class: %s holds \"%.40s\", which is not a display kind; accepted: %s. Not applied: the display's "
	         "default (%s) is used until it is corrected",
	         source, value, U_VIEW_CLASS_NAMES, class_names[U_VIEW_CLASS_SCREEN]);
}

bool
u_view_class_select(const char *env_value,
                    const char *file_path,
                    enum u_view_class *out,
                    enum u_view_class_source *out_source,
                    char *message,
                    size_t message_size)
{
	if (message != NULL && message_size > 0)
		message[0] = '\0';
	*out = U_VIEW_CLASS_SCREEN;
	*out_source = U_VIEW_CLASS_FROM_DEFAULT;
	if (env_value != NULL && env_value[0] != '\0') {
		if (u_view_class_parse(env_value, out)) {
			*out_source = U_VIEW_CLASS_FROM_ENVIRONMENT;
			return true;
		}
		*out = U_VIEW_CLASS_SCREEN;
		say(message, message_size, VIEW_CLASS_ENV, env_value);
		return false;
	}
	if (file_path != NULL) {
		FILE *f = fopen(file_path, "r");
		if (f != NULL) {
			char line[64] = {0};
			bool got = fgets(line, sizeof(line), f) != NULL;
			fclose(f);
			if (got) {
				size_t n = strlen(line);
				while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
					line[--n] = '\0';
			}
			if (!got || line[0] == '\0')
				return true; // an empty file selects nothing: the display's default
			if (u_view_class_parse(line, out)) {
				*out_source = U_VIEW_CLASS_FROM_FILE;
				return true;
			}
			*out = U_VIEW_CLASS_SCREEN;
			say(message, message_size, file_path, line);
			return false;
		}
	}
	return true;
}

bool
u_view_class_select_for_process(enum u_view_class *out,
                                enum u_view_class_source *out_source,
                                char *message,
                                size_t message_size)
{
	char path[1024];
	const char *file = NULL;
#ifndef XRT_OS_WINDOWS
	int n = u_file_get_path_in_config_dir(VIEW_CLASS_FILE, path, sizeof(path));
	if (n > 0 && (size_t)n < sizeof(path))
		file = path;
#else
	(void)path; // Windows: the environment only, until the machine-wide configuration path is decided
#endif
	return u_view_class_select(getenv(VIEW_CLASS_ENV), file, out, out_source, message, message_size);
}
