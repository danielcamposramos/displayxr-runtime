// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tell the person when the runtime cannot declare its window (sparky-stereo-problems).
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_problems.h"

#include "util/u_logging.h"

#ifdef DXR_SPARKY_PROBLEMS

#include <sparky-stereo-problems.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define PACKAGE "displayxr-runtime"
#define KEY "declare"
#define KEY_LAYOUT "layout"

typedef int (*keep_fn)(int, const char *, const char *, const char *, const char *const *, int, const char *);
typedef int (*clear_fn)(int, const char *, const char *);
typedef const char *(*error_fn)(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool opened;
static keep_fn keep;
static clear_fn clear;
static error_fn error_en;

//! @p quiet: nothing is being told (the window is declared), so an absent engine is an INFO line, not an error.
static bool
engine_open(bool quiet)
{
	if (opened) {
		return keep != NULL;
	}
	opened = true;
	const enum u_logging_level level = quiet ? U_LOGGING_INFO : U_LOGGING_ERROR;
	void *lib = dlopen(DXR_SPARKY_PROBLEMS_LIBRARY, RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		U_LOG(level, "sparky-stereo-problems: %s: %s", dlerror(),
		      quiet ? "nothing to end (the window is declared)" : "the person cannot be told why the window is not declared");
		return false;
	}
	keep_fn k = (keep_fn)dlsym(lib, "ssp_keep");
	clear_fn c = (clear_fn)dlsym(lib, "ssp_clear");
	error_en = (error_fn)dlsym(lib, "ssp_error_en");
	if (k == NULL || c == NULL || error_en == NULL) {
		U_LOG(level, "sparky-stereo-problems: %s lacks ssp_keep, ssp_clear or ssp_error_en: %s",
		      DXR_SPARKY_PROBLEMS_LIBRARY,
		      quiet ? "nothing to end (the window is declared)" : "the person cannot be told why the window is not declared");
		return false;
	}
	keep = k;
	clear = c;
	return true;
}

static const char *
message_of(enum comp_vk_native_declare_problem kind)
{
	// The package's own messages (debian/lang, built into catalogs); LOCAL1 is the title.
	switch (kind) {
	case COMP_VK_NATIVE_DECLARE_NO_ANNOUNCEMENT: return "LOCAL2";
	case COMP_VK_NATIVE_DECLARE_WINDOW_NOT_DECLARABLE: return "LOCAL3";
	case COMP_VK_NATIVE_DECLARE_BUILT_WITHOUT_SCANNER: return "LOCAL4";
	default: return "LOCAL5";
	}
}

static const char *
english_of(enum comp_vk_native_declare_problem kind)
{
	// Said when the package's catalog is not there; the catalog says it in the person's language.
	switch (kind) {
	case COMP_VK_NATIVE_DECLARE_NO_ANNOUNCEMENT:
		return "DisplayXR cannot show this program in 3D: the desktop does not announce the stereo declaration "
		       "that DisplayXR needs, so the program is shown as its two views side by side, flat. Use Stereo "
		       "KWin (the desktop of Sparky Stereo OS), then start the program again.";
	case COMP_VK_NATIVE_DECLARE_WINDOW_NOT_DECLARABLE:
		return "DisplayXR cannot show this program in 3D: its window is one DisplayXR does not declare to the "
		       "desktop, so the program is shown as its two views side by side, flat.";
	case COMP_VK_NATIVE_DECLARE_BUILT_WITHOUT_SCANNER:
		return "DisplayXR cannot show this program in 3D: this build of the runtime has no Wayland "
		       "declaration, so the program is shown as its two views side by side, flat. Install the "
		       "displayxr-runtime package of your distribution.";
	default:
		return "DisplayXR cannot show this program in 3D: the declaration to the desktop could not be "
		       "made, so the program is shown as its two views side by side, flat.";
	}
}

//! Writes the JSON string of @p in, escaped, into @p out; false when it does not fit.
static bool
json_string(char *out, size_t size, const char *in)
{
	size_t n = 0;
	for (; *in != '\0'; in++) {
		const unsigned char ch = (unsigned char)*in;
		const char *esc = ch == '"' ? "\\\"" : ch == '\\' ? "\\\\" : NULL;
		if (esc == NULL && ch < 0x20) {
			esc = " ";
		}
		const size_t add = esc != NULL ? strlen(esc) : 1;
		if (n + add + 1 > size) {
			return false;
		}
		if (esc != NULL) {
			memcpy(out + n, esc, add);
		} else {
			out[n] = (char)ch;
		}
		n += add;
	}
	out[n] = '\0';
	return true;
}

void
comp_vk_native_problems_declare(enum comp_vk_native_declare_problem kind, const char *detail)
{
	pthread_mutex_lock(&lock);
	if (kind == COMP_VK_NATIVE_DECLARE_OK) {
		// A problem an earlier start of any program kept ends here: the engine ends nothing that is not there.
		if (engine_open(true) && clear(SSP_PERSON, PACKAGE, KEY) != 0) {
			U_LOG_E("sparky-stereo-problems: the window is declared but the earlier problem could not be "
			        "ended: %s",
			        error_en());
		}
		pthread_mutex_unlock(&lock);
		return;
	}
	if (!engine_open(false)) {
		pthread_mutex_unlock(&lock);
		return;
	}
	char text[1024];
	char escaped[2048];
	char details[3200];
	snprintf(text, sizeof text, "%s DisplayXR says: %s", english_of(kind), detail != NULL ? detail : "");
	if (!json_string(escaped, sizeof escaped, text) ||
	    snprintf(details, sizeof details, "{\"text_en\":\"%s\"}", escaped) >=
	        (int)sizeof details) {
		U_LOG_E("sparky-stereo-problems: the problem text does not fit");
		pthread_mutex_unlock(&lock);
		return;
	}
	const char *args[1] = {detail != NULL ? detail : ""};
	if (keep(SSP_PERSON, PACKAGE, KEY, message_of(kind), args, 1, details) != 0) {
		U_LOG_E("sparky-stereo-problems: the problem could not be kept: %s", error_en());
	}
	pthread_mutex_unlock(&lock);
}

void
comp_vk_native_problems_layout(bool unsupported, const char *detail)
{
	pthread_mutex_lock(&lock);
	if (!unsupported) {
		if (engine_open(true) && clear(SSP_PERSON, PACKAGE, KEY_LAYOUT) != 0) {
			U_LOG_E("sparky-stereo-problems: the layout is laid in both pictures but the earlier problem could "
			        "not be ended: %s",
			        error_en());
		}
		pthread_mutex_unlock(&lock);
		return;
	}
	if (!engine_open(false)) {
		pthread_mutex_unlock(&lock);
		return;
	}
	char text[1024];
	char escaped[2048];
	char details[3200];
	snprintf(text, sizeof text,
	         "DisplayXR cannot lay this program's 2D content in both pictures of its 3D window, so the 2D content "
	         "can be missing or out of place in one of them. Use a version of the program that places its 2D "
	         "content as ordinary rectangles over the 3D. DisplayXR says: %s",
	         detail != NULL ? detail : "");
	if (!json_string(escaped, sizeof escaped, text) ||
	    snprintf(details, sizeof details, "{\"text_en\":\"%s\"}", escaped) >= (int)sizeof details) {
		U_LOG_E("sparky-stereo-problems: the problem text does not fit");
		pthread_mutex_unlock(&lock);
		return;
	}
	const char *args[1] = {detail != NULL ? detail : ""};
	if (keep(SSP_PERSON, PACKAGE, KEY_LAYOUT, "LOCAL6", args, 1, details) != 0) {
		U_LOG_E("sparky-stereo-problems: the problem could not be kept: %s", error_en());
	}
	pthread_mutex_unlock(&lock);
}

#else // !DXR_SPARKY_PROBLEMS

void
comp_vk_native_problems_declare(enum comp_vk_native_declare_problem kind, const char *detail)
{
	(void)kind;
	(void)detail;
}

void
comp_vk_native_problems_layout(bool unsupported, const char *detail)
{
	(void)unsupported;
	(void)detail;
}

#endif
