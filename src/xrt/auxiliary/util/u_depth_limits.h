// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  How far a legacy application's picture may sink and pop, and the camera rig that keeps to it.
 *
 * ORDER 478 milestone 2b (Sparky Stereo OS). The limits are the desktop's: Stereo KWin announces, for the output a
 * window mainly sits on, how far behind and in front of the screen anything may go (kde_stereo_depth_v1.depth_limits,
 * since version 5), "in pixels of a view 1920 wide ... the user's limits built on the output's display: a headset gets
 * more separation, a framed 3D screen less". A program reads them and draws inside them, so a page's popped element,
 * the desktop's popped menu and a game's nearest thing agree on one screen. The same limits give a legacy
 * application's camera rig its separation and screen plane, here.
 *
 * The order of importance is the browser's and the desktop's: the person's choice, built on the display. The person's
 * choice is a class (tv or headset, the browser's flat modes), a strength in percent, a screen plane distance and, for
 * either limit, an absolute value in pixels that replaces it. Nothing here is a number of ours: with no limits and no
 * absolute value there is nothing to build on, and the caller says so.
 *
 * The closed form. A point Z in front of the camera (world units, metres for OpenVR) has, as a fraction of the canvas
 * width and positive behind the screen plane, the disparity of docs/specs/runtime/legacy-camera-profile.md,
 *
 *   p(Z) = k (c - 1/Z),   k = ipd * m2v * eyeSeparation / (2 tan(vFov/2) aspect),   c = 1/D   (D = the plane's distance),
 *
 * so a point at infinity has p = k c. Infinity at the sunk limit s (sunk pixels / 1920) is k c = s, hence k = s D and
 *
 *   ipd = s D (2 tan(vFov/2) aspect) / (m2v eyeSeparation),   p(Z) = s (1 - D/Z).
 *
 * That is the browser's far parallax f (its sunk limit) and its screen plane (dom.vr.flat.screen-plane-mm). The popped
 * limit (popped pixels / 1920 = q) is met at Z = D s / (s + q); anything nearer pops past it, because the camera does
 * what the geometry says and a finished picture is never shifted. That distance is the scene's nearest working
 * distance: a person who wants a scene to reach nearer moves the plane (D = Znear (s + q) / s), and
 * u_depth_limits_plane_for_nearest() gives the plane for a nearest distance.
 */

#pragma once

#include "util/u_view_class.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Pixels of a view 1920 wide: the unit of the desktop's depth settings and of kde_stereo_depth_v1.
#define U_DEPTH_LIMITS_VIEW_WIDTH 1920.0

//! The browser's screen plane when the person has not set one (dom.vr.flat.screen-plane-mm 0): 1 m.
#define U_DEPTH_LIMITS_DEFAULT_PLANE_MM 1000.0

//! One pair of limits, pixels of a view 1920 wide. popped_px < 0: not known.
struct u_depth_limits
{
	double sunk_px;
	double popped_px;
};

/*
 *
 * What the compositor announced. One process-wide copy, written by whoever speaks to the compositor, read by the
 * sessions. Safe from any thread.
 *
 */

//! The compositor announced these limits for the output the window mainly sits on. Bumps the serial when they change.
void
u_depth_limits_publish(double sunk_px, double popped_px);

//! The compositor no longer announces limits (the surface is gone, or it never could).
void
u_depth_limits_withdraw(void);

//! The announced limits, if any; @p out_serial changes whenever they do.
bool
u_depth_limits_announced(struct u_depth_limits *out, uint64_t *out_serial);

/*
 *
 * The person's choice.
 *
 */

struct u_depth_settings
{
	enum u_view_class cls;           //!< tv, headset or auto
	double strength_percent;         //!< of the limits, 100 = as they are; 0 switches depth off
	double plane_mm;                 //!< the screen plane's distance, 0 = the default
	double sunk_px;                  //!< replaces the sunk limit when above 0
	double popped_px;                //!< replaces the popped limit when above 0
	uint64_t serial;                 //!< changes whenever the settings are read again and differ
};

//! The settings with nothing chosen: auto, 100 %, the default plane, no absolute values.
void
u_depth_settings_default(struct u_depth_settings *s);

/*!
 * Read the person's choices: the class (DXR_VIEW_CLASS, else the file view-class), and DXR_VIEW_DEPTH_STRENGTH,
 * DXR_VIEW_SCREEN_PLANE_MM, DXR_VIEW_SUNK_PX and DXR_VIEW_POPPED_PX, each else the same key in the file `view-depth`
 * of the configuration directory (lines `key=value`: depth-strength, screen-plane-mm, sunk-px, popped-px).
 * The file is read again by u_depth_settings_refresh() when it changes.
 * A value that is wrong (not a number, below 0, a strength over 1000, an unknown key or class) is REFUSED, never
 * silently dropped: @p message says which source held what and what is used instead (the value left at its default).
 * Returns false if anything was refused.
 */
bool
u_depth_settings_load(struct u_depth_settings *s, char *message, size_t message_size);

//! The same from text instead of the process: @p env_get is called with a variable name and returns its value or NULL;
//! @p view_depth_text is the content of the view-depth file or NULL; @p class_file_text that of view-class.
bool
u_depth_settings_parse(struct u_depth_settings *s,
                       const char *(*env_get)(const char *name, void *user),
                       void *user,
                       const char *view_depth_text,
                       const char *class_file_text,
                       char *message,
                       size_t message_size);

/*
 *
 * Limits to the rig.
 *
 */

//! Where the limits in force came from.
enum u_depth_source
{
	U_DEPTH_SOURCE_NONE = 0,   //!< there are none
	U_DEPTH_SOURCE_COMPOSITOR, //!< the compositor's, for the output
	U_DEPTH_SOURCE_PERSON,     //!< the person's absolute values (no compositor limits)
};

//! The limits in force: the compositor's with the person's choices on top.
struct u_depth_resolved
{
	enum u_depth_source source;
	double sunk_px;   //!< pixels of a view 1920 wide; 0 = depth off
	double popped_px; //!< negative = not known
	double plane_m;
	bool sunk_replaced;   //!< the person's absolute sunk value replaced the compositor's
	bool popped_replaced;
};

/*!
 * The person's choices on top of the compositor's limits (@p announced, or NULL when it announced none). Returns false
 * and leaves @p out at U_DEPTH_SOURCE_NONE when there is nothing to build on: no compositor limits and no absolute
 * sunk value.
 */
bool
u_depth_resolve(const struct u_depth_settings *s, const struct u_depth_limits *announced, struct u_depth_resolved *out);

//! What the rig is solved for.
struct u_depth_rig_input
{
	double sunk_px;
	double popped_px; //!< negative = not known
	double plane_m;
	double aspect;         //!< canvas width / height
	double half_tan_vfov;  //!< tan(vFov / 2)
	double m2v;            //!< metres to the app's world units
	double eye_separation; //!< the display's reported eye separation, metres
};

struct u_depth_rig
{
	float ipd_factor;
	float inv_convergence_distance; //!< 1 / plane, in world units
	double far_fraction;            //!< p(infinity), a fraction of the canvas width, positive behind the plane
	double nearest_m;               //!< where the popped limit is met; < 0 when the popped limit is not known
	double popped_fraction;         //!< the popped limit as a fraction of the canvas width, < 0 when not known
};

//! False for an input the closed form cannot take (a non-positive aspect, field of view, m2v, eye separation or plane).
bool
u_depth_rig_solve(const struct u_depth_rig_input *in, struct u_depth_rig *out);

//! The disparity of a point @p z_world in front of the camera for a rig, as a fraction of the canvas width, positive
//! behind the plane (the closed form above).
double
u_depth_rig_disparity(const struct u_depth_rig_input *in, const struct u_depth_rig *rig, double z_world);

//! The plane distance that makes the popped limit meet @p nearest_m; < 0 when the limits cannot give one.
double
u_depth_limits_plane_for_nearest(double sunk_px, double popped_px, double nearest_m);

#ifdef __cplusplus
}
#endif
