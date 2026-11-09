// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Dev probe for the Linux gamepad backend (evdev).
 *
 * Runs the real backend as the hub does (init, poll at 250 Hz) and prints what the provider derives from the pad,
 * one line when the state changes and one every 500 ms, for the seconds given (default 15):
 *
 *   STATE connected=1 pads=2 speaking="DXR test pad B" buttons=0x0001 lx=0.000 ly=0.000 rx=0.000 ry=0.000 lt=0.000 rt=0.000
 *   NAV   x=0.000 y=0.000 z=-0.250 yaw=0.0 pitch=0.0
 *
 * NAV is the navigation pose the hub integrates from the state with the default mapping table (left stick = move,
 * right stick = look), so a stick held forward moves z negative. Not a test (it needs a pad); the test that drives
 * it with a virtual pad is gamepad_evdev_uinput.py. Target gamepad_evdev_probe, never built by default.
 */

#include "gamepad_backend.h"
#include "gamepad_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t
now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int
main(int argc, char **argv)
{
	const double seconds = argc > 1 ? atof(argv[1]) : 15.0;
	if (!gamepad_backend_available()) {
		printf("DECLINED %s\n", gamepad_backend_name());
		fflush(stdout);
		return 3;
	}
	if (!gamepad_backend_init()) {
		printf("INITFAILED %s\n", gamepad_backend_name());
		return 4;
	}
	printf("BACKEND %s\n", gamepad_backend_name());
	fflush(stdout);
	const struct gamepad_map_config cfg = GAMEPAD_MAP_CONFIG_DEFAULT;
	struct gamepad_nav_pose nav = GAMEPAD_NAV_POSE_IDENTITY;
	struct gamepad_state last;
	memset(&last, 0, sizeof(last));
	last.connected = false;
	bool first = true;
	const int64_t start = now_ns();
	int64_t prev = start, printed = 0;
	while ((now_ns() - start) < (int64_t)(seconds * 1e9)) {
		struct gamepad_state st;
		memset(&st, 0, sizeof(st));
		gamepad_backend_poll(&st);
		const int64_t t = now_ns();
		struct gamepad_nav_velocity vel;
		gamepad_map_nav_velocity(&st, &gamepad_map_default_table, &cfg, &vel);
		double dt = (double)(t - prev) * 1e-9;
		if (dt > 0.1) {
			dt = 0.1;
		}
		gamepad_map_integrate(&nav, &vel, dt);
		prev = t;
		const bool changed = first || st.connected != last.connected || st.buttons != last.buttons ||
		                     fabsf(st.left_x - last.left_x) > 1e-3f || fabsf(st.left_y - last.left_y) > 1e-3f ||
		                     fabsf(st.right_x - last.right_x) > 1e-3f || fabsf(st.right_y - last.right_y) > 1e-3f ||
		                     fabsf(st.left_trigger - last.left_trigger) > 1e-3f ||
		                     fabsf(st.right_trigger - last.right_trigger) > 1e-3f;
		if (changed || t - printed > 500000000LL) {
			printf("STATE connected=%d buttons=0x%04x lx=%.3f ly=%.3f rx=%.3f ry=%.3f lt=%.3f rt=%.3f\n",
			       st.connected ? 1 : 0, st.buttons, st.left_x, st.left_y, st.right_x, st.right_y,
			       st.left_trigger, st.right_trigger);
			printf("NAV   x=%.3f y=%.3f z=%.3f yaw=%.1f pitch=%.1f\n", nav.x, nav.y, nav.z,
			       nav.yaw_rad * 180.0 / 3.14159265358979323846, nav.pitch_rad * 180.0 / 3.14159265358979323846);
			fflush(stdout);
			last = st;
			printed = t;
			first = false;
		}
		struct timespec s = {0, 4000000};
		nanosleep(&s, NULL);
	}
	gamepad_backend_fini();
	return 0;
}
