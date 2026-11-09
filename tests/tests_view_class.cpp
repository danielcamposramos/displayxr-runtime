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

} // namespace

TEST_CASE("The selector takes exactly the browser's two flat modes", "[view-class]")
{
	u_view_class cls = U_VIEW_CLASS_HEADSET;
	REQUIRE(u_view_class_parse("tv", &cls));
	CHECK(cls == U_VIEW_CLASS_TV);
	REQUIRE(u_view_class_parse("headset", &cls));
	CHECK(cls == U_VIEW_CLASS_HEADSET);
	cls = U_VIEW_CLASS_HEADSET;
	for (const char *bad : {"", "TV", "Tv", "tv ", " headset", "screen", "auto", "headset2", "flat", "0", "-1"}) {
		CAPTURE(bad);
		CHECK_FALSE(u_view_class_parse(bad, &cls));
		CHECK(cls == U_VIEW_CLASS_HEADSET);
	}
	CHECK_FALSE(u_view_class_parse(nullptr, &cls));
	CHECK(std::string(u_view_class_name(U_VIEW_CLASS_TV)) == "tv");
	CHECK(std::string(u_view_class_name(U_VIEW_CLASS_AUTO)) == "auto");
	CHECK(std::string(u_view_class_name(U_VIEW_CLASS_HEADSET)) == "headset");
}

TEST_CASE("The person's selector: environment, then file, then auto", "[view-class]")
{
	std::string headset_file = write_file("dxr-view-class-headset", "headset\n");
	Selected s = select(nullptr, nullptr);
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_AUTO);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	s = select(nullptr, headset_file.c_str());
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_HEADSET);
	CHECK(s.source == U_VIEW_CLASS_FROM_FILE);
	s = select("tv", headset_file.c_str());
	CHECK(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_TV);
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
	CHECK(s.cls == U_VIEW_CLASS_AUTO);
	CHECK(s.source == U_VIEW_CLASS_FROM_DEFAULT);
	CHECK(s.message.find("DXR_VIEW_CLASS") != std::string::npos);
	CHECK(s.message.find("headest") != std::string::npos);
	CHECK(s.message.find("tv, headset") != std::string::npos);
	CHECK(s.message.find("Not applied") != std::string::npos);
	// the file holds a wrong value
	std::string bad_file = write_file("dxr-view-class-bad", "screen\n");
	s = select(nullptr, bad_file.c_str());
	CHECK_FALSE(s.ok);
	CHECK(s.cls == U_VIEW_CLASS_AUTO);
	CHECK(s.message.find(bad_file) != std::string::npos);
	CHECK(s.message.find("\"screen\"") != std::string::npos);
	// a number is not a display kind either
	s = select("2.5", nullptr);
	CHECK_FALSE(s.ok);
	CHECK(s.message.find("2.5") != std::string::npos);
	std::filesystem::remove(headset_file);
	std::filesystem::remove(bad_file);
}

TEST_CASE("A class is a factor on the compositor's limits: auto and tv 1, headset 2.5 (libstereo-policy's kinds)",
          "[view-class]")
{
	CHECK(u_view_class_factor(U_VIEW_CLASS_AUTO) == 1.0);
	CHECK(u_view_class_factor(U_VIEW_CLASS_TV) == 1.0);
	CHECK(u_view_class_factor(U_VIEW_CLASS_HEADSET) == 2.5);
}
