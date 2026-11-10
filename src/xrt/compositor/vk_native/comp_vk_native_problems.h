// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tell the person when the runtime cannot declare its window to the desktop.
 *
 * The runtime hands a stereo desktop compositor the two views side by side and
 * declares the window once (comp_vk_native_wl_stereo, the X11 property). When
 * the declaration cannot be made the views are shown flat, side by side, and
 * the reason used to reach the log only. A distribution that has a place where
 * the person is told about failures (Sparky Stereo OS: sparky-stereo-problems,
 * its session notifier and the Problems page of System Settings) builds this
 * with DXR_SPARKY_PROBLEMS: the reason is kept there until a declaring start
 * clears it. Without the option every call is a no-op and nothing is linked.
 *
 * The engine's library is opened with dlopen at the first call, so the runtime
 * keeps its list of linked libraries (and starts) where the engine is absent;
 * then the log says, as an error, that the person could not be told.
 *
 * @ingroup comp_vk_native
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//! Why the window is not declared. The kinds are the messages a catalog says in
//! the person's language; the detail is the runtime's own English sentence.
enum comp_vk_native_declare_problem
{
	//! Declared, or nothing to declare (a 2D mode, DXR_STEREO_DECLARE=0 chosen by the person).
	COMP_VK_NATIVE_DECLARE_OK = 0,
	//! The desktop announces no stereo declaration (or an older one than the runtime needs).
	COMP_VK_NATIVE_DECLARE_NO_ANNOUNCEMENT,
	//! The window is one the runtime does not declare (the app's own X11 window, a child window).
	COMP_VK_NATIVE_DECLARE_WINDOW_NOT_DECLARABLE,
	//! This runtime was built without wayland-scanner.
	COMP_VK_NATIVE_DECLARE_BUILT_WITHOUT_SCANNER,
	//! Any other reason the declaration could not be made.
	COMP_VK_NATIVE_DECLARE_OTHER,
};

/*!
 * Keeps the problem for @p kind with @p detail (the person's problem, key
 * "declare"; kept again with the same kind it keeps its first time), or, for
 * COMP_VK_NATIVE_DECLARE_OK, ends it. Called when the answer changes, never per
 * frame. Failures of the engine itself are logged as errors.
 */
void
comp_vk_native_problems_declare(enum comp_vk_native_declare_problem kind, const char *detail);

#ifdef __cplusplus
}
#endif
