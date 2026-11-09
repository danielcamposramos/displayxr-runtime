// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "util/u_view_class.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using Catch::Approx;

namespace {

struct Selected
{
	bool ok;
	u_view_class cls;
	u_view_class_source source;
	std::string message;
};

Selected
select(const char *env, const char *file)
{
	Selected s{};
	char message[320];
	s.ok = u_view_class_select(env, file, &s.cls, &s.source, message, sizeof(message));
	s.message = message;
	return s;
}

std::string
write_file(const char *name, const std::string &text)
{
	std::filesystem::path p = std::filesystem::temp_directory_path() / name;
	std::ofstream(p, std::ios::binary) << text;
	return p.string();
}

//! The closed form of docs/specs/runtime/legacy-camera-profile.md, as a fraction of the canvas width (positive =
//! behind the screen plane): ipd * m2v * (convergence - 1/Z) * eyeSeparation / (2 * tan(vFov/2) * aspect).
double
disparity(const u_camera_profile &p, double z)
{
	const double eye_separation = 0.060; // sim_display's reported eye separation, metres
	const double aspect = 16.0 / 9.0;
	return p.ipd_factor * p.m2v * (p.inv_convergence_distance - 1.0 / z) * eye_separation /
	       (2.0 * p.half_tan_vfov * aspect);
}

} // namespace

TEST_CASE("The selector takes exactly the two display kinds", "[view-class]")
{
	u_view_class cls = U_VIEW_CLASS_HEADSET;
	REQUIRE(u_view_class_parse("screen", &cls));
	CHECK(cls == U_VIEW_CLASS_SCREEN);
	REQUIRE(u_view_class_parse("headset", &cls));
	CHECK(cls == U_VIEW_CLASS_HEADSET);
	cls = U_VIEW_CLASS_HEADSET;
	for (const char *bad : {"", "Screen", "SCREEN", "screen ", " headset", "tv", "headset2", "flat", "0", "-1"}) {
		CAPTURE(bad);
		CHECK_FALSE(u_view_class_parse(bad, &cls));
		CHECK(cls == U_VIEW_CLASS_HEADSET);
	}
	CHECK_FALSE(u_view_class_parse(nullptr, &cls));
	CHECK(std::string(u_view_class_name(U_VIEW_CLASS_SCREEN)) == "screen");
	CHECK(std::string(u_view_class_name(U_VIEW_CLASS_HEADSET)) == "headset");
}

TEST_CASE("The person's selector: environment, then file, then the display's default", "[view-class]")
{
	std::string headset_file = write_file("dxr-view-class-headset", "headset\n");
	Selected s = select(nullptr, nullptr);
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_SCREEN);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	s = select(nullptr, headset_file.c_str());
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_HEADSET);
	CHECK(s.source == U_VIEW_CLASS_FROM_FILE);
	s = select("screen", headset_file.c_str());
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_SCREEN);
	CHECK(s.source == U_VIEW_CLASS_FROM_ENVIRONMENT);
	s = select("", headset_file.c_str()); // an empty variable is no choice
	CHECK(s.cls == U_VIEW_CLASS_HEADSET);
	CHECK(s.source == U_VIEW_CLASS_FROM_FILE);
	s = select(nullptr, "/nonexistent/dxr-view-class"); // no file is no choice
	CHECK(s.ok);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	std::string empty_file = write_file("dxr-view-class-empty", "");
	s = select(nullptr, empty_file.c_str());
	CHECK(s.ok);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	std::filesystem::remove(headset_file);
	std::filesystem::remove(empty_file);
}

TEST_CASE("A selector value that is not a display kind is refused and said", "[view-class]")
{
	std::string headset_file = write_file("dxr-view-class-valid", "headset\n");
	// the environment holds a wrong value: refused, the file's valid value is NOT used in its place
	Selected s = select("headest", headset_file.c_str());
	CHECK_FALSE(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_SCREEN);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	CHECK(s.message.find("DXR_VIEW_CLASS") != std::string::npos);
	CHECK(s.message.find("headest") != std::string::npos);
	CHECK(s.message.find("screen, headset") != std::string::npos);
	CHECK(s.message.find("Not applied") != std::string::npos);
	// the file holds a wrong value
	std::string bad_file = write_file("dxr-view-class-bad", "tv\n");
	s = select(nullptr, bad_file.c_str());
	CHECK_FALSE(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_SCREEN);
	CHECK(s.message.find(bad_file) != std::string::npos);
	CHECK(s.message.find("\"tv\"") != std::string::npos);
	// a number is not a display kind either
	s = select("2.5", nullptr);
	CHECK_FALSE(s.ok);
	CHECK(s.message.find("2.5") != std::string::npos);
	std::filesystem::remove(headset_file);
	std::filesystem::remove(bad_file);
}

TEST_CASE("The class defaults: a headset takes 2.5 times the separation of a screen, the pop is 0.4 of the sunk side",
          "[view-class]")
{
	u_camera_profile screen{}, headset{};
	u_view_class_profile(U_VIEW_CLASS_SCREEN, &screen);
	u_view_class_profile(U_VIEW_CLASS_HEADSET, &headset);
	// libstereo-policy's kinds: STEREO_POLICY_DISPLAY_HEADSET takes 2.5 times the separation of a framed screen
	CHECK(headset.ipd_factor / screen.ipd_factor == Approx(2.5).epsilon(0.001));
	CHECK(screen.inv_convergence_distance == headset.inv_convergence_distance);
	CHECK(screen.parallax_factor == headset.parallax_factor);
	CHECK(screen.half_tan_vfov == headset.half_tan_vfov);
	CHECK(screen.m2v == headset.m2v);

	// the measurement behind the numbers (fractions of the canvas width, positive behind the screen plane)
	const double sunk = disparity(screen, 1e9);
	const double pop = -disparity(screen, 0.8);
	CHECK(sunk == Approx(0.020).margin(0.0003)); // 2 % at infinity, the edition's browser on a 3D screen
	CHECK(pop == Approx(0.008).margin(0.0003));  // at 0.8 m
	CHECK(pop / sunk == Approx(0.079 / 0.198).margin(0.01)); // libstereo-policy: popped 0.079 deg, sunk 0.198 deg
	CHECK(disparity(headset, 1e9) == Approx(0.050).margin(0.0006));
	CHECK(-disparity(headset, 0.8) == Approx(0.020).margin(0.0006));
	// every depth, not only these two, scales by the ratio of the separations
	for (double z : {0.5, 0.8, 1.2, 2.0, 6.0, 30.0})
		CHECK(disparity(headset, z) == Approx(2.5 * disparity(screen, z)).epsilon(0.001));
	// the runtime's comfort rule (dxr_rig_clamp_for_comfort: ipd * m2v * invd * N <= 1) holds at a 0.6 m viewer
	for (const u_camera_profile *p : {&screen, &headset}) {
		const double ipd = p->ipd_factor > 1.0f ? p->ipd_factor : 1.0f;
		CHECK(p->inv_convergence_distance <= 1.0 / (ipd * p->m2v * 0.6) + 1e-6);
	}
}
