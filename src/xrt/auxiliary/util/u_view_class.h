// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The kind of display the person says a legacy application's two views are made for. The names are the browser's
//! flat modes (`dom.vr.flat.mode`: tv, headset), so one idea has one name. The person chooses; the runtime never
//! decides it from the output (one desktop can drive a 3D television and a viewer at the same time).
enum u_view_class
{
	//! Nothing chosen: the depth limits of the output the compositor announces, as they are.
	U_VIEW_CLASS_AUTO = 0,
	//! A framed 3D screen (television, projector, monitor, anaglyph): the limits as the compositor announces them.
	U_VIEW_CLASS_TV = 1,
	//! A headset (or a phone in a viewer): libstereo-policy's headset factor over a framed screen's limits.
	U_VIEW_CLASS_HEADSET = 2,
};

//! The names the selector takes.
#define U_VIEW_CLASS_NAMES "tv, headset"

//! libstereo-policy's kind factors (stereo_policy_display_depth, STEREO_POLICY_DISPLAY_*): framed 1.0, headset 2.5.
#define U_VIEW_CLASS_TV_FACTOR 1.0
#define U_VIEW_CLASS_HEADSET_FACTOR 2.5

enum u_view_class_source
{
	U_VIEW_CLASS_FROM_DEFAULT = 0, //!< nothing set it: the limits as announced (U_VIEW_CLASS_AUTO)
	U_VIEW_CLASS_FROM_ENVIRONMENT, //!< DXR_VIEW_CLASS
	U_VIEW_CLASS_FROM_FILE,        //!< view-class in the runtime's configuration directory
};

//! Parse one selector value (exact, lower case). False for anything else; @p out is untouched.
bool
u_view_class_parse(const char *name, enum u_view_class *out);

//! The selector's name, as u_view_class_parse takes it; "auto" for U_VIEW_CLASS_AUTO.
const char *
u_view_class_name(enum u_view_class cls);

//! The factor a class puts on the limits the compositor announces (auto and tv 1, headset 2.5).
double
u_view_class_factor(enum u_view_class cls);

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
