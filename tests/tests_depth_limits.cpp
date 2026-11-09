// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The depth limits and the camera rig that keeps to them (ORDER 478 M2b).
 *
 * The closed form is the legacy camera profile's (docs/specs/runtime/legacy-camera-profile.md, "Disparity does not
 * depend on the display"): the disparity of a point Z in front of the camera, as a fraction of the canvas width and
 * positive behind the screen plane, is k (c - 1/Z) with k = ipd m2v eyeSeparation / (2 tan(vFov/2) aspect). The
 * limits are Stereo KWin's (kde_stereo_depth_v1.depth_limits: pixels of a view 1920 wide), so infinity must land on
 * the sunk limit and the point where the popped limit is met must land on it.
 */

#include "catch_amalgamated.hpp"
#include "util/u_depth_limits.h"

#include <cmath>
#include <map>
#include <string>

using Catch::Approx;

namespace {

u_depth_rig_input
input(double sunk, double popped, double plane_m = 1.0)
{
	u_depth_rig_input in{};
	in.sunk_px = sunk;
	in.popped_px = popped;
	in.plane_m = plane_m;
	in.aspect = 16.0 / 9.0;
	in.half_tan_vfov = 0.3249;
	in.m2v = 1.0;
	in.eye_separation = 0.060;
	return in;
}

struct Env
{
	std::map<std::string, std::string> v;
};
const char *
env_get(const char *name, void *user)
{
	auto *e = static_cast<Env *>(user);
	auto it = e->v.find(name);
	return it == e->v.end() ? nullptr : it->second.c_str();
}

} // namespace

TEST_CASE("Infinity lands on the sunk limit and the popped limit is met at D s / (s + q)", "[depth-limits]")
{
	for (double sunk : {4.0, 10.0, 11.8, 25.0, 100.0}) {
		for (double popped : {0.0, 4.0, 4.7, 12.0}) {
			CAPTURE(sunk, popped);
			const u_depth_rig_input in = input(sunk, popped);
			u_depth_rig rig;
			REQUIRE(u_depth_rig_solve(&in, &rig));
			CHECK(u_depth_rig_disparity(&in, &rig, 1e12) == Approx(sunk / 1920.0).epsilon(1e-5));
			CHECK(rig.far_fraction == Approx(sunk / 1920.0));
			REQUIRE(rig.nearest_m > 0.0);
			CHECK(u_depth_rig_disparity(&in, &rig, rig.nearest_m) == Approx(-popped / 1920.0).margin(1e-6));
			// the browser's form, f (1 - plane / z), for every distance
			for (double z : {0.5, 0.8, 1.0, 1.5, 3.0, 10.0})
				CHECK(u_depth_rig_disparity(&in, &rig, z) == Approx(sunk / 1920.0 * (1.0 - 1.0 / z)).margin(1e-6));
		}
	}
}

TEST_CASE("The plane is the person's: it moves the nearest working distance, not the far limit", "[depth-limits]")
{
	for (double plane : {0.6, 1.0, 1.5, 3.0}) {
		const u_depth_rig_input in = input(10.0, 4.0, plane);
		u_depth_rig rig;
		REQUIRE(u_depth_rig_solve(&in, &rig));
		CHECK(u_depth_rig_disparity(&in, &rig, 1e12) == Approx(10.0 / 1920.0).epsilon(1e-5));
		CHECK(rig.nearest_m == Approx(plane * 10.0 / 14.0));
		CHECK(u_depth_limits_plane_for_nearest(10.0, 4.0, rig.nearest_m) == Approx(plane));
		CHECK(rig.inv_convergence_distance == Approx(1.0 / plane));
	}
}

TEST_CASE("The metres-to-world scale cancels in the separation and scales the plane", "[depth-limits]")
{
	u_depth_rig_input a = input(10.0, 4.0), b = a;
	b.m2v = 2.0;
	u_depth_rig ra, rb;
	REQUIRE(u_depth_rig_solve(&a, &ra));
	REQUIRE(u_depth_rig_solve(&b, &rb));
	CHECK(ra.ipd_factor == Approx(rb.ipd_factor));
	CHECK(rb.inv_convergence_distance == Approx(ra.inv_convergence_distance / 2.0));
	// the same fraction of the canvas at the same distance in metres (world distance = metres * m2v)
	CHECK(u_depth_rig_disparity(&a, &ra, 5.0) == Approx(u_depth_rig_disparity(&b, &rb, 10.0)));
}

TEST_CASE("The display's reported eye separation is divided out, so the limits hold on any display", "[depth-limits]")
{
	for (double es : {0.055, 0.060, 0.065, 0.070}) {
		u_depth_rig_input in = input(10.0, 4.0);
		in.eye_separation = es;
		u_depth_rig rig;
		REQUIRE(u_depth_rig_solve(&in, &rig));
		CHECK(u_depth_rig_disparity(&in, &rig, 1e12) == Approx(10.0 / 1920.0).epsilon(1e-5));
	}
	for (double aspect : {16.0 / 9.0, 16.0 / 10.0, 4.0 / 3.0}) {
		u_depth_rig_input in = input(10.0, 4.0);
		in.aspect = aspect;
		u_depth_rig rig;
		REQUIRE(u_depth_rig_solve(&in, &rig));
		CHECK(u_depth_rig_disparity(&in, &rig, 1e12) == Approx(10.0 / 1920.0).epsilon(1e-5));
	}
}

TEST_CASE("Control: the prediction with the other limits does not match", "[depth-limits]")
{
	const u_depth_rig_input a = input(10.0, 4.0), b = input(20.0, 8.0);
	u_depth_rig ra;
	REQUIRE(u_depth_rig_solve(&a, &ra));
	// a rig solved for a is read with b's prediction: off by the whole difference
	const double predicted_for_b = 20.0 / 1920.0;
	const double measured = u_depth_rig_disparity(&a, &ra, 1e12);
	CHECK(std::fabs(measured - predicted_for_b) > 0.004);
	(void)b;
}

TEST_CASE("The closed form refuses inputs it cannot take", "[depth-limits]")
{
	u_depth_rig rig;
	u_depth_rig_input in = input(10.0, 4.0);
	in.eye_separation = 0.0;
	CHECK_FALSE(u_depth_rig_solve(&in, &rig));
	in = input(10.0, 4.0);
	in.aspect = 0.0;
	CHECK_FALSE(u_depth_rig_solve(&in, &rig));
	in = input(10.0, 4.0);
	in.plane_m = -1.0;
	CHECK_FALSE(u_depth_rig_solve(&in, &rig));
	in = input(-1.0, 4.0);
	CHECK_FALSE(u_depth_rig_solve(&in, &rig));
	in = input(10.0, -1.0); // popped not known: the rig is still solved, the nearest distance is not
	REQUIRE(u_depth_rig_solve(&in, &rig));
	CHECK(rig.nearest_m < 0.0);
	in = input(0.0, 0.0); // depth off
	REQUIRE(u_depth_rig_solve(&in, &rig));
	CHECK(rig.ipd_factor == 0.0f);
}

TEST_CASE("The person's choices on top of the compositor's limits", "[depth-limits]")
{
	u_depth_settings s;
	u_depth_settings_default(&s);
	u_depth_limits ann{10.0, 4.0};
	u_depth_resolved r;

	// nothing chosen: the limits as announced
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.source == U_DEPTH_SOURCE_COMPOSITOR);
	CHECK(r.sunk_px == 10.0);
	CHECK(r.popped_px == 4.0);
	CHECK(r.plane_m == Approx(1.0));

	// tv is the same, headset is 2.5 times
	s.cls = U_VIEW_CLASS_TV;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.sunk_px == 10.0);
	s.cls = U_VIEW_CLASS_HEADSET;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.sunk_px == Approx(25.0));
	CHECK(r.popped_px == Approx(10.0));

	// strength multiplies both; 0 switches depth off
	s.strength_percent = 50.0;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.sunk_px == Approx(12.5));
	s.cls = U_VIEW_CLASS_AUTO;
	s.strength_percent = 0.0;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.sunk_px == 0.0);

	// an absolute value replaces its limit, the other stays the compositor's
	s.strength_percent = 100.0;
	s.sunk_px = 30.0;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.sunk_px == 30.0);
	CHECK(r.sunk_replaced);
	CHECK(r.popped_px == 4.0);
	CHECK_FALSE(r.popped_replaced);

	// the plane
	u_depth_settings_default(&s);
	s.plane_mm = 1500.0;
	REQUIRE(u_depth_resolve(&s, &ann, &r));
	CHECK(r.plane_m == Approx(1.5));
}

TEST_CASE("With no compositor limits there is nothing to build on unless the person gave an absolute sunk value",
          "[depth-limits]")
{
	u_depth_settings s;
	u_depth_settings_default(&s);
	u_depth_resolved r;
	CHECK_FALSE(u_depth_resolve(&s, nullptr, &r));
	CHECK(r.source == U_DEPTH_SOURCE_NONE);
	s.cls = U_VIEW_CLASS_HEADSET; // a class and a strength have nothing to scale
	s.strength_percent = 200.0;
	CHECK_FALSE(u_depth_resolve(&s, nullptr, &r));
	s.popped_px = 5.0; // a popped value alone is not a sunk limit
	CHECK_FALSE(u_depth_resolve(&s, nullptr, &r));
	s.sunk_px = 20.0;
	REQUIRE(u_depth_resolve(&s, nullptr, &r));
	CHECK(r.source == U_DEPTH_SOURCE_PERSON);
	CHECK(r.sunk_px == 20.0); // absolute values are final: class and strength do not touch them
	CHECK(r.popped_px == 5.0);
	s.popped_px = 0.0;
	REQUIRE(u_depth_resolve(&s, nullptr, &r));
	CHECK(r.popped_px < 0.0);
}

TEST_CASE("Settings: the environment, then the file, a wrong value refused and said", "[depth-limits]")
{
	char msg[1024];
	u_depth_settings s;
	Env env;
	// nothing: the defaults
	CHECK(u_depth_settings_parse(&s, env_get, &env, nullptr, nullptr, msg, sizeof(msg)));
	CHECK(s.cls == U_VIEW_CLASS_AUTO);
	CHECK(s.strength_percent == 100.0);
	CHECK(s.plane_mm == 0.0);
	CHECK(s.sunk_px == 0.0);
	CHECK(msg[0] == '\0');

	// the file
	const char *file = "depth-strength=60\nscreen-plane-mm=1200\nsunk-px=30\npopped-px=12\n";
	CHECK(u_depth_settings_parse(&s, env_get, &env, file, "headset\n", msg, sizeof(msg)));
	CHECK(s.cls == U_VIEW_CLASS_HEADSET);
	CHECK(s.strength_percent == 60.0);
	CHECK(s.plane_mm == 1200.0);
	CHECK(s.sunk_px == 30.0);
	CHECK(s.popped_px == 12.0);

	// the environment beats the file, key by key; the class too
	env.v["DXR_VIEW_DEPTH_STRENGTH"] = "150";
	env.v["DXR_VIEW_CLASS"] = "tv";
	CHECK(u_depth_settings_parse(&s, env_get, &env, file, "headset\n", msg, sizeof(msg)));
	CHECK(s.strength_percent == 150.0);
	CHECK(s.cls == U_VIEW_CLASS_TV);
	CHECK(s.plane_mm == 1200.0);

	// a wrong environment value is refused, not read past to the file
	env.v.clear();
	env.v["DXR_VIEW_DEPTH_STRENGTH"] = "lots";
	CHECK_FALSE(u_depth_settings_parse(&s, env_get, &env, file, nullptr, msg, sizeof(msg)));
	CHECK(s.strength_percent == 100.0);
	CHECK(std::string(msg).find("DXR_VIEW_DEPTH_STRENGTH") != std::string::npos);
	CHECK(std::string(msg).find("lots") != std::string::npos);
	CHECK(std::string(msg).find("not applied") != std::string::npos);

	// the others in the same call still apply
	CHECK(s.sunk_px == 30.0);

	// out of range, negative, trailing text, empty
	for (const char *bad : {"-1", "1001", "50x", "nan", "inf", "1e999"}) {
		env.v.clear();
		env.v["DXR_VIEW_DEPTH_STRENGTH"] = bad;
		CAPTURE(bad);
		CHECK_FALSE(u_depth_settings_parse(&s, env_get, &env, nullptr, nullptr, msg, sizeof(msg)));
		CHECK(s.strength_percent == 100.0);
	}
	env.v.clear();
	env.v["DXR_VIEW_DEPTH_STRENGTH"] = "";
	CHECK(u_depth_settings_parse(&s, env_get, &env, nullptr, nullptr, msg, sizeof(msg))); // an empty variable is no choice

	// a wrong class, in the environment and in the file
	env.v.clear();
	env.v["DXR_VIEW_CLASS"] = "screen";
	CHECK_FALSE(u_depth_settings_parse(&s, env_get, &env, nullptr, "headset\n", msg, sizeof(msg)));
	CHECK(s.cls == U_VIEW_CLASS_AUTO);
	CHECK(std::string(msg).find("tv, headset") != std::string::npos);
	env.v.clear();
	CHECK_FALSE(u_depth_settings_parse(&s, env_get, &env, nullptr, "flat\n", msg, sizeof(msg)));
	CHECK(std::string(msg).find("view-class") != std::string::npos);

	// a key the file does not know is named, the rest applies
	CHECK_FALSE(u_depth_settings_parse(&s, env_get, &env, "strength=10\nsunk-px=12\n", nullptr, msg, sizeof(msg)));
	CHECK(std::string(msg).find("strength=10") != std::string::npos);
	CHECK(s.sunk_px == 12.0);
	// comments and blank lines are fine
	CHECK(u_depth_settings_parse(&s, env_get, &env, "# my limits\n\nsunk-px=12\n", nullptr, msg, sizeof(msg)));
}

TEST_CASE("What the compositor announced, and when it stops", "[depth-limits]")
{
	u_depth_limits_withdraw();
	u_depth_limits l{};
	uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
	CHECK_FALSE(u_depth_limits_announced(&l, &s0));
	u_depth_limits_publish(10.0, 4.0);
	REQUIRE(u_depth_limits_announced(&l, &s1));
	CHECK(l.sunk_px == 10.0);
	CHECK(l.popped_px == 4.0);
	CHECK(s1 != s0);
	u_depth_limits_publish(10.0, 4.0); // the same again is not a change
	u_depth_limits_announced(&l, &s2);
	CHECK(s2 == s1);
	u_depth_limits_publish(20.0, 8.0);
	REQUIRE(u_depth_limits_announced(&l, &s3));
	CHECK(l.sunk_px == 20.0);
	CHECK(s3 != s2);
	u_depth_limits_withdraw();
	CHECK_FALSE(u_depth_limits_announced(&l, &s0));
	CHECK(s0 != s3);
}
