// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "util/u_camera_profile.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The kind of display a legacy application's two views are made for. The person chooses it; the runtime never
//! decides it from the output (one desktop can drive a 3D television and a viewer at the same time).
enum u_view_class
{
	//! A 3D screen (television, projector, monitor, anaglyph): gentle separation, the nearest things pop a little.
	U_VIEW_CLASS_SCREEN = 0,
	//! A headset (or a phone in a viewer): the display's own eye separation, a larger one than a screen takes.
	U_VIEW_CLASS_HEADSET = 1,
};

//! The names the selector takes, in order of enum u_view_class.
#define U_VIEW_CLASS_NAMES "screen, headset"

enum u_view_class_source
{
	U_VIEW_CLASS_FROM_DEFAULT = 0, //!< nothing set it: the default of the display (U_VIEW_CLASS_SCREEN)
	U_VIEW_CLASS_FROM_ENVIRONMENT, //!< DXR_VIEW_CLASS
	U_VIEW_CLASS_FROM_FILE,        //!< view-class in the runtime's configuration directory
};

//! Parse one selector value (exact, lower case). False for anything else; @p out is untouched.
bool
u_view_class_parse(const char *name, enum u_view_class *out);

//! The selector's name, as u_view_class_parse takes it.
const char *
u_view_class_name(enum u_view_class cls);

/*!
 * The camera-rig profile a class stands for: the separation (ipdFactor) and the zero-parallax distance
 * (convergence) that differ between kinds of display; everything else keeps the qwerty defaults.
 */
void
u_view_class_profile(enum u_view_class cls, struct u_camera_profile *out);

/*!
 * Resolve the person's selector, in this order: the environment value @p env_value (DXR_VIEW_CLASS), then the
 * first line of the file @p file_path (NULL = no file), then the default of the display. A value that is not a
 * class name is REFUSED: @p out is the default, the result is false and @p message says which source held it,
 * what it was and which names are accepted, so the caller can tell the person. Never silent.
 */
bool
u_view_class_select(const char *env_value,
                    const char *file_path,
                    enum u_view_class *out,
                    enum u_view_class_source *out_source,
                    char *message,
                    size_t message_size);

//! u_view_class_select with the process environment and view-class in the configuration directory.
bool
u_view_class_select_for_process(enum u_view_class *out,
                                enum u_view_class_source *out_source,
                                char *message,
                                size_t message_size);

#ifdef __cplusplus
}
#endif
