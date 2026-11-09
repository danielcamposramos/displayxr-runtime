// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The person's choice of display kind for legacy applications' views.
 *
 * ORDER 478 milestones 2 and 2b (Sparky Stereo OS). The two names are the browser's flat modes (tv, headset). A
 * class is a factor on the depth limits the compositor announces for the output (u_depth_limits.h), libstereo-policy's
 * kind factors: a headset takes 2.5 times the separation of a framed screen. No number of the camera rig is chosen
 * here: the rig follows from the limits.
 */

#include "util/u_view_class.h"

#include "xrt/xrt_config_os.h"

#include "util/u_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_CLASS_FILE "view-class"
#define VIEW_CLASS_ENV "DXR_VIEW_CLASS"

static const char *const class_names[] = {"auto", "tv", "headset"};

bool
u_view_class_parse(const char *name, enum u_view_class *out)
{
	if (name == NULL)
		return false;
	for (size_t i = 1; i < sizeof(class_names) / sizeof(class_names[0]); i++) {
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
	return class_names[cls == U_VIEW_CLASS_TV ? 1 : cls == U_VIEW_CLASS_HEADSET ? 2 : 0];
}

double
u_view_class_factor(enum u_view_class cls)
{
	return cls == U_VIEW_CLASS_HEADSET ? U_VIEW_CLASS_HEADSET_FACTOR : U_VIEW_CLASS_TV_FACTOR;
}

static void
say(char *message, size_t size, const char *source, const char *value)
{
	if (message == NULL || size == 0)
		return;
	snprintf(message, size,
	         "view class: %s holds \"%.40s\", which is not a display kind; accepted: %s. Not applied: the "
	         "compositor's limits (%s) are used until it is corrected",
	         source, value, U_VIEW_CLASS_NAMES, class_names[U_VIEW_CLASS_AUTO]);
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
	*out = U_VIEW_CLASS_AUTO;
	*out_source = U_VIEW_CLASS_FROM_DEFAULT;
	if (env_value != NULL && env_value[0] != '\0') {
		if (u_view_class_parse(env_value, out)) {
			*out_source = U_VIEW_CLASS_FROM_ENVIRONMENT;
			return true;
		}
		*out = U_VIEW_CLASS_AUTO;
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
			*out = U_VIEW_CLASS_AUTO;
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
