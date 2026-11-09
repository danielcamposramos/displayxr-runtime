// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux pad reader: the decoder on synthetic evdev events, the device layer on pipes.
 *
 * Nothing here opens a device node or /dev/uinput. The decoder (`gamepad_evdev_decode.c`) is fed arrays of
 * `struct input_event`; the device layer (`gamepad_evdev_core.c`) gets a scripted node list, pipes for the
 * pads' file descriptors and a clock the test moves. What the kernel does with a real device is
 * `tests/gamepad_evdev_uinput.sh` (a virtual pad through /dev/uinput, run where that is allowed).
 *
 * The mapping is the kernel's gamepad protocol (Documentation/input/gamepad.rst): BTN_SOUTH, BTN_EAST,
 * BTN_NORTH, BTN_WEST by position; "for ABS values negative is left/up" for the sticks and the d-pad; ABS_HAT2*
 * and the legacy ABS_Z / ABS_RZ for the lower triggers.
 */

#include "catch_amalgamated.hpp"

#if defined(__linux__) && !defined(__ANDROID__)

#include "gamepad_evdev_core.h"
#include "gamepad_evdev_decode.h"

#include <linux/input.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr float kEps = 1e-5f;

input_event
ev(uint16_t type, uint16_t code, int32_t value)
{
	input_event e = {};
	e.type = type;
	e.code = code;
	e.value = value;
	return e;
}
input_event
key(uint16_t code, int v)
{
	return ev(EV_KEY, code, v);
}
input_event
abs_(uint16_t code, int v)
{
	return ev(EV_ABS, code, v);
}
input_event
syn()
{
	return ev(EV_SYN, SYN_REPORT, 0);
}

gamepad_evdev_caps
xbox_caps()
{
	gamepad_evdev_caps c = {};
	auto set = [&](int axis, int lo, int hi) {
		c.axis[axis].present = true;
		c.axis[axis].min = lo;
		c.axis[axis].max = hi;
	};
	set(GP_EVDEV_AXIS_X, -32768, 32767);
	set(GP_EVDEV_AXIS_Y, -32768, 32767);
	set(GP_EVDEV_AXIS_RX, -32768, 32767);
	set(GP_EVDEV_AXIS_RY, -32768, 32767);
	set(GP_EVDEV_AXIS_Z, 0, 1023);
	set(GP_EVDEV_AXIS_RZ, 0, 1023);
	set(GP_EVDEV_AXIS_HAT0X, -1, 1);
	set(GP_EVDEV_AXIS_HAT0Y, -1, 1);
	return c;
}

void
init_xbox(gamepad_evdev_decoder &d)
{
	const gamepad_evdev_caps c = xbox_caps();
	gamepad_evdev_decoder_init(&d, &c);
}

} // namespace

TEST_CASE("decoder: buttons by the kernel's positions, one report at a time")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event batch[] = {key(BTN_SOUTH, 1), key(BTN_NORTH, 1), key(BTN_TL, 1), key(BTN_START, 1), syn()};
	REQUIRE(gamepad_evdev_decoder_feed(&d, batch, 5));
	CHECK(d.state.buttons == (GAMEPAD_BTN_A | GAMEPAD_BTN_X | GAMEPAD_BTN_SHOULDER_LEFT | GAMEPAD_BTN_MENU));
	const input_event rel[] = {key(BTN_SOUTH, 0), key(BTN_EAST, 1), key(BTN_WEST, 1), key(BTN_SELECT, 1),
	                           key(BTN_MODE, 1),  key(BTN_THUMBL, 1), syn()};
	REQUIRE(gamepad_evdev_decoder_feed(&d, rel, 7));
	CHECK(d.state.buttons == (GAMEPAD_BTN_X | GAMEPAD_BTN_SHOULDER_LEFT | GAMEPAD_BTN_MENU | GAMEPAD_BTN_B |
	                          GAMEPAD_BTN_Y | GAMEPAD_BTN_VIEW | GAMEPAD_BTN_GUIDE | GAMEPAD_BTN_THUMB_LEFT));
}

TEST_CASE("decoder: the state changes at SYN_REPORT, never half way through a batch")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event half[] = {key(BTN_SOUTH, 1), abs_(ABS_X, 32767)};
	CHECK_FALSE(gamepad_evdev_decoder_feed(&d, half, 2));
	CHECK(d.state.buttons == 0);
	CHECK(d.state.left_x == Catch::Approx(0.0f).margin(kEps));
	const input_event end[] = {syn()};
	CHECK(gamepad_evdev_decoder_feed(&d, end, 1));
	CHECK(d.state.buttons == GAMEPAD_BTN_A);
	CHECK(d.state.left_x == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("decoder: sticks are -1..1 about the middle of their range, and Y is negated to +Y up")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	// the kernel: negative is left and up
	const input_event b[] = {abs_(ABS_X, -32768), abs_(ABS_Y, -32768), abs_(ABS_RX, 32767), abs_(ABS_RY, 32767), syn()};
	REQUIRE(gamepad_evdev_decoder_feed(&d, b, 5));
	CHECK(d.state.left_x == Catch::Approx(-1.0f).margin(1e-4));
	CHECK(d.state.left_y == Catch::Approx(1.0f).margin(1e-4)); // up
	CHECK(d.state.right_x == Catch::Approx(1.0f).margin(1e-4));
	CHECK(d.state.right_y == Catch::Approx(-1.0f).margin(1e-4)); // down
	// the middle of an asymmetric range is 0 within one step of it
	const input_event m[] = {abs_(ABS_X, -1), abs_(ABS_Y, -1), syn()};
	gamepad_evdev_decoder_feed(&d, m, 3);
	CHECK(std::fabs(d.state.left_x) < 1e-4f);
	CHECK(std::fabs(d.state.left_y) < 1e-4f);
	// a pad with a 0..255 range (a cheap HID pad) centres at 127.5
	gamepad_evdev_caps c = xbox_caps();
	c.axis[GP_EVDEV_AXIS_X].min = 0;
	c.axis[GP_EVDEV_AXIS_X].max = 255;
	gamepad_evdev_decoder e;
	gamepad_evdev_decoder_init(&e, &c);
	const input_event r[] = {abs_(ABS_X, 255), syn()};
	gamepad_evdev_decoder_feed(&e, r, 2);
	CHECK(e.state.left_x == Catch::Approx(1.0f).margin(1e-5));
	const input_event r0[] = {abs_(ABS_X, 0), syn()};
	gamepad_evdev_decoder_feed(&e, r0, 2);
	CHECK(e.state.left_x == Catch::Approx(-1.0f).margin(1e-5));
}

TEST_CASE("decoder: an out of range value is held to the range")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event b[] = {abs_(ABS_X, 900000), abs_(ABS_Z, -50), syn()};
	gamepad_evdev_decoder_feed(&d, b, 3);
	CHECK(d.state.left_x <= 1.0f);
	CHECK(d.state.left_trigger >= 0.0f);
}

TEST_CASE("decoder: the lower triggers, legacy axes, the protocol's hats and the digital keys")
{
	{
		gamepad_evdev_decoder d;
		init_xbox(d);
		const input_event b[] = {abs_(ABS_Z, 1023), abs_(ABS_RZ, 512), syn()};
		gamepad_evdev_decoder_feed(&d, b, 3);
		CHECK(d.state.left_trigger == Catch::Approx(1.0f).margin(1e-5));
		CHECK(d.state.right_trigger == Catch::Approx(512.0f / 1023.0f).margin(1e-4));
	}
	{
		gamepad_evdev_caps c = xbox_caps();
		c.axis[GP_EVDEV_AXIS_Z].present = false;
		c.axis[GP_EVDEV_AXIS_RZ].present = false;
		c.axis[GP_EVDEV_AXIS_HAT2X] = {true, 0, 255};
		c.axis[GP_EVDEV_AXIS_HAT2Y] = {true, 0, 255};
		gamepad_evdev_decoder d;
		gamepad_evdev_decoder_init(&d, &c);
		const input_event b[] = {abs_(ABS_HAT2Y, 255), abs_(ABS_HAT2X, 51), syn()}; // HAT2Y is the left one
		gamepad_evdev_decoder_feed(&d, b, 3);
		CHECK(d.state.left_trigger == Catch::Approx(1.0f).margin(1e-5));
		CHECK(d.state.right_trigger == Catch::Approx(0.2f).margin(1e-5));
	}
	{
		gamepad_evdev_decoder d;
		init_xbox(d);
		const input_event b[] = {key(BTN_TL2, 1), syn()};
		gamepad_evdev_decoder_feed(&d, b, 2);
		CHECK(d.state.left_trigger == 1.0f);
		CHECK(d.state.right_trigger == 0.0f);
	}
}

TEST_CASE("decoder: a pad with no ABS_RX has its right stick on ABS_Z and ABS_RZ and no analog triggers")
{
	gamepad_evdev_caps c = xbox_caps();
	c.axis[GP_EVDEV_AXIS_RX].present = false;
	c.axis[GP_EVDEV_AXIS_RY].present = false;
	c.axis[GP_EVDEV_AXIS_Z] = {true, 0, 255};
	c.axis[GP_EVDEV_AXIS_RZ] = {true, 0, 255};
	gamepad_evdev_decoder d;
	gamepad_evdev_decoder_init(&d, &c);
	const input_event b[] = {abs_(ABS_Z, 255), abs_(ABS_RZ, 0), key(BTN_TR2, 1), syn()};
	gamepad_evdev_decoder_feed(&d, b, 4);
	CHECK(d.state.right_x == Catch::Approx(1.0f).margin(1e-5));
	CHECK(d.state.right_y == Catch::Approx(1.0f).margin(1e-5)); // 0 is up
	CHECK(d.state.left_trigger == 0.0f);
	CHECK(d.state.right_trigger == 1.0f); // the digital key only
}

TEST_CASE("decoder: the d-pad as keys and as a hat, either presses")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event hat[] = {abs_(ABS_HAT0X, -1), abs_(ABS_HAT0Y, -1), syn()}; // negative is left and up
	gamepad_evdev_decoder_feed(&d, hat, 3);
	CHECK(d.state.buttons == (GAMEPAD_BTN_DPAD_LEFT | GAMEPAD_BTN_DPAD_UP));
	const input_event mix[] = {abs_(ABS_HAT0X, 1), abs_(ABS_HAT0Y, 0), key(BTN_DPAD_DOWN, 1), syn()};
	gamepad_evdev_decoder_feed(&d, mix, 4);
	CHECK(d.state.buttons == (GAMEPAD_BTN_DPAD_RIGHT | GAMEPAD_BTN_DPAD_DOWN));
	const input_event off[] = {abs_(ABS_HAT0X, 0), key(BTN_DPAD_DOWN, 0), syn()};
	gamepad_evdev_decoder_feed(&d, off, 3);
	CHECK(d.state.buttons == 0);
}

TEST_CASE("decoder: autorepeat (value 2) keeps a key down, a release lets it go")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event b[] = {key(BTN_SOUTH, 1), syn(), key(BTN_SOUTH, 2), syn()};
	gamepad_evdev_decoder_feed(&d, b, 4);
	CHECK(d.state.buttons == GAMEPAD_BTN_A);
	const input_event r[] = {key(BTN_SOUTH, 0), syn()};
	gamepad_evdev_decoder_feed(&d, r, 2);
	CHECK(d.state.buttons == 0);
}

TEST_CASE("decoder: SYN_DROPPED discards the lost batch and asks for a resync")
{
	gamepad_evdev_decoder d;
	init_xbox(d);
	const input_event ok[] = {key(BTN_SOUTH, 1), syn()};
	gamepad_evdev_decoder_feed(&d, ok, 2);
	REQUIRE(d.state.buttons == GAMEPAD_BTN_A);
	// the kernel overran: what follows up to the next SYN_REPORT is a torn batch
	const input_event lost[] = {ev(EV_SYN, SYN_DROPPED, 0), key(BTN_SOUTH, 0), key(BTN_EAST, 1), syn()};
	CHECK_FALSE(gamepad_evdev_decoder_feed(&d, lost, 4));
	CHECK(d.needs_resync);
	CHECK(d.state.buttons == GAMEPAD_BTN_A); // unchanged until the resync
	// the device layer reads the real state (B is down, A is up) and sets it
	gamepad_evdev_decoder_set_key(&d, BTN_SOUTH, false);
	gamepad_evdev_decoder_set_key(&d, BTN_EAST, true);
	gamepad_evdev_decoder_commit(&d);
	CHECK(d.state.buttons == GAMEPAD_BTN_B);
	// and events after the lost batch count again
	d.needs_resync = false;
	const input_event next[] = {key(BTN_NORTH, 1), syn()};
	CHECK(gamepad_evdev_decoder_feed(&d, next, 2));
	CHECK(d.state.buttons == (GAMEPAD_BTN_B | GAMEPAD_BTN_X));
}

TEST_CASE("decoder: a code the protocol does not use is ignored, and an absent axis is not read")
{
	gamepad_evdev_caps c = xbox_caps();
	c.axis[GP_EVDEV_AXIS_RX].present = false;
	c.axis[GP_EVDEV_AXIS_RY].present = false;
	c.axis[GP_EVDEV_AXIS_Z].present = false;
	c.axis[GP_EVDEV_AXIS_RZ].present = false;
	gamepad_evdev_decoder d;
	gamepad_evdev_decoder_init(&d, &c);
	const input_event b[] = {key(KEY_A, 1), key(BTN_TRIGGER, 1), abs_(ABS_RX, 32767), abs_(ABS_MISC, 5), ev(EV_MSC, MSC_SCAN, 1), syn()};
	gamepad_evdev_decoder_feed(&d, b, 6);
	CHECK(d.state.buttons == 0);
	CHECK(d.state.right_x == 0.0f);
}

TEST_CASE("decoder: which nodes are pads")
{
	gamepad_evdev_caps c = xbox_caps();
	CHECK(gamepad_evdev_is_gamepad(&c, true, false));
	CHECK_FALSE(gamepad_evdev_is_gamepad(&c, false, false)); // a flight stick or a touchpad: no BTN_SOUTH
	CHECK_FALSE(gamepad_evdev_is_gamepad(&c, true, true));   // a mouse that also has a button
	c.axis[GP_EVDEV_AXIS_Y].present = false;
	CHECK_FALSE(gamepad_evdev_is_gamepad(&c, true, false)); // no left stick
}

// ---- the device layer, on pipes ----

namespace {

struct fake_pad
{
	std::string path;
	std::string name;
	int rd = -1;
	int wr = -1;
	bool is_pad = true;
	bool deny = false;
	gamepad_evdev_caps caps = xbox_caps();
	input_event resync_keys[4] = {};
	std::vector<std::pair<int, int>> resync_down; // key codes held when the device is re-read
};

struct fake_system
{
	std::vector<fake_pad *> nodes;
	int64_t now = 1000000000LL;
	bool changed_flag = false;
	bool list_fails = false;
	int lists = 0;
	std::vector<std::string> said;
	std::vector<int> closed;
	fake_pad *by_fd(int fd)
	{
		for (auto *n : nodes)
			if (n->rd == fd)
				return n;
		return nullptr;
	}
};

fake_system *g_fs = nullptr;

int
f_list(void *, void (*emit)(void *, const char *), void *user)
{
	g_fs->lists++;
	if (g_fs->list_fails)
		return -1;
	for (auto *n : g_fs->nodes)
		emit(user, n->path.c_str());
	return (int)g_fs->nodes.size();
}
int
f_open(void *, const char *path)
{
	for (auto *n : g_fs->nodes) {
		if (n->path == path) {
			if (n->deny)
				return -EACCES;
			return n->rd;
		}
	}
	return -ENOENT;
}
gamepad_evdev_probe
f_probe(void *, int fd, gamepad_evdev_caps *caps, char *name, size_t size)
{
	fake_pad *n = g_fs->by_fd(fd);
	if (!n || !n->is_pad)
		return GP_EVDEV_PROBE_NOT_A_PAD;
	*caps = n->caps;
	snprintf(name, size, "%s", n->name.c_str());
	return GP_EVDEV_PROBE_PAD;
}
ssize_t
f_read(void *, int fd, void *buf, size_t size)
{
	return read(fd, buf, size);
}
void
f_resync(void *, int fd, gamepad_evdev_decoder *d)
{
	fake_pad *n = g_fs->by_fd(fd);
	if (!n)
		return;
	for (auto &kv : n->resync_down)
		gamepad_evdev_decoder_set_key(d, (uint16_t)kv.first, kv.second != 0);
}
void
f_close(void *, int fd)
{
	g_fs->closed.push_back(fd);
}
bool
f_changed(void *)
{
	bool c = g_fs->changed_flag;
	g_fs->changed_flag = false;
	return c;
}
int64_t
f_now(void *)
{
	return g_fs->now;
}
void
f_say(void *, int, const char *line)
{
	g_fs->said.push_back(line);
}

gamepad_evdev_ops
fake_ops()
{
	gamepad_evdev_ops o = {};
	o.list = f_list;
	o.open = f_open;
	o.probe = f_probe;
	o.read = f_read;
	o.resync = f_resync;
	o.close = f_close;
	o.changed = f_changed;
	o.now_ns = f_now;
	o.say = f_say;
	return o;
}

void
make(fake_pad &p, const char *path, const char *name)
{
	int fds[2];
	REQUIRE(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
	p.rd = fds[0];
	p.wr = fds[1];
	p.path = path;
	p.name = name;
}

void
send(fake_pad &p, std::vector<input_event> evs)
{
	REQUIRE(write(p.wr, evs.data(), evs.size() * sizeof(input_event)) == (ssize_t)(evs.size() * sizeof(input_event)));
}

void
unplug(fake_pad &p)
{
	close(p.wr);
	p.wr = -1;
}

bool
said(const char *needle)
{
	for (auto &s : g_fs->said)
		if (s.find(needle) != std::string::npos)
			return true;
	return false;
}

} // namespace

TEST_CASE("device layer: no pads means not connected, a pad plugged in later is found")
{
	fake_system fs;
	g_fs = &fs;
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	CHECK_FALSE(st.connected);

	fake_pad a;
	make(a, "/dev/input/event7", "Pad A");
	fs.nodes.push_back(&a);
	// the list is read again when the system says it changed
	fs.changed_flag = true;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.connected);
	CHECK(said("pad attached: Pad A"));
	send(a, {key(BTN_SOUTH, 1), syn()});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_A);
	gamepad_evdev_core_fini(&core);
}

TEST_CASE("device layer: while none is attached the list is read again at most once a second")
{
	fake_system fs;
	g_fs = &fs;
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(fs.lists == 1);
	fs.now += 100000000LL;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(fs.lists == 1);
	fs.now += 1000000000LL;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(fs.lists == 2);
}

TEST_CASE("device layer: two pads, the one touched last speaks")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a, b;
	make(a, "/dev/input/event5", "Pad A");
	make(b, "/dev/input/event6", "Pad B");
	fs.nodes = {&a, &b};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(gamepad_evdev_core_pad_count(&core) == 2);

	fs.now += 10;
	send(a, {key(BTN_SOUTH, 1), syn()});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_A);
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad A");

	fs.now += 10;
	send(b, {key(BTN_EAST, 1), syn()});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_B);
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad B");

	fs.now += 10;
	send(a, {abs_(ABS_X, 32767), syn()});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.left_x == Catch::Approx(1.0f).margin(1e-4));
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad A");
	gamepad_evdev_core_fini(&core);
}

static input_event
syn_at(int64_t ms)
{
	input_event e = syn();
	e.input_event_sec = ms / 1000;
	e.input_event_usec = (ms % 1000) * 1000;
	return e;
}

TEST_CASE("device layer: two pads read in one poll, the report stamped later speaks, whichever is read last")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a, b;
	make(a, "/dev/input/event5", "Pad A");
	make(b, "/dev/input/event6", "Pad B");
	fs.nodes = {&a, &b};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(gamepad_evdev_core_pad_count(&core) == 2);

	// A is read first and was touched after B: A speaks.
	send(b, {key(BTN_EAST, 1), syn_at(1000)});
	send(a, {key(BTN_SOUTH, 1), syn_at(1001)});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_A);
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad A");

	// And the other way round: B was touched after A.
	send(a, {key(BTN_SOUTH, 0), syn_at(2000)});
	send(b, {key(BTN_EAST, 0), key(BTN_NORTH, 1), syn_at(2001)});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_X);
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad B");
	gamepad_evdev_core_fini(&core);
}

TEST_CASE("device layer: a pad that goes away is closed and the other takes over")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a, b;
	make(a, "/dev/input/event5", "Pad A");
	make(b, "/dev/input/event6", "Pad B");
	fs.nodes = {&a, &b};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	fs.now += 10;
	send(b, {key(BTN_NORTH, 1), syn()});
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad B");

	unplug(b); // end of file on the pipe, as ENODEV on the node
	fs.now += 10;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(gamepad_evdev_core_pad_count(&core) == 1);
	CHECK(st.connected);
	CHECK(std::string(gamepad_evdev_core_speaking_name(&core)) == "Pad A");
	CHECK(said("Pad B is gone"));
	CHECK(fs.closed.size() == 1);

	unplug(a);
	fs.now += 10;
	gamepad_evdev_core_poll(&core, &st);
	CHECK_FALSE(st.connected);
	CHECK(st.buttons == 0);
	CHECK(gamepad_evdev_core_pad_count(&core) == 0);
}

TEST_CASE("device layer: a pad whose node left the list is dropped even if its descriptor still reads")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a;
	make(a, "/dev/input/event5", "Pad A");
	fs.nodes = {&a};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(st.connected);
	fs.nodes.clear();
	fs.changed_flag = true;
	gamepad_evdev_core_poll(&core, &st);
	CHECK_FALSE(st.connected);
	CHECK(said("its node left"));
}

TEST_CASE("device layer: nodes that are not pads are looked at once, and a node returning is looked at again")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad kbd, pad;
	make(kbd, "/dev/input/event0", "Keyboard");
	kbd.is_pad = false;
	make(pad, "/dev/input/event1", "Pad");
	fs.nodes = {&kbd, &pad};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(gamepad_evdev_core_pad_count(&core) == 1);
	// the keyboard descriptor was closed once; a second scan does not open it again
	const size_t closedAfterFirst = fs.closed.size();
	fs.changed_flag = true;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(fs.closed.size() == closedAfterFirst);
	gamepad_evdev_core_fini(&core);
}

TEST_CASE("device layer: nodes nobody may read are said once, and a node that becomes readable is taken")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a;
	make(a, "/dev/input/event5", "Pad A");
	a.deny = true;
	fs.nodes = {&a};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	CHECK_FALSE(st.connected);
	CHECK(said("no input node can be read"));
	const size_t said1 = fs.said.size();
	fs.now += 1100000000LL;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(fs.said.size() == said1); // not said again
	// udev's ACL arrives
	a.deny = false;
	fs.changed_flag = true;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.connected);
}

TEST_CASE("device layer: an unreadable input directory is said once")
{
	fake_system fs;
	g_fs = &fs;
	fs.list_fails = true;
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	fs.now += 2000000000LL;
	gamepad_evdev_core_poll(&core, &st);
	CHECK_FALSE(st.connected);
	int n = 0;
	for (auto &s : fs.said)
		if (s.find("input directory cannot be read") != std::string::npos)
			n++;
	CHECK(n == 1);
}

TEST_CASE("device layer: a pad found already holding a button starts from its real state")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a;
	make(a, "/dev/input/event5", "Pad A");
	a.resync_down = {{BTN_SOUTH, 1}};
	fs.nodes = {&a};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_A);
	gamepad_evdev_core_fini(&core);
}

TEST_CASE("device layer: events lost to SYN_DROPPED are replaced by the device's state")
{
	fake_system fs;
	g_fs = &fs;
	fake_pad a;
	make(a, "/dev/input/event5", "Pad A");
	fs.nodes = {&a};
	gamepad_evdev_core core;
	gamepad_evdev_ops ops = fake_ops();
	gamepad_evdev_core_init(&core, &ops);
	gamepad_state st;
	gamepad_evdev_core_poll(&core, &st);
	send(a, {key(BTN_SOUTH, 1), syn()});
	gamepad_evdev_core_poll(&core, &st);
	REQUIRE(st.buttons == GAMEPAD_BTN_A);
	// A is released and B pressed, but the kernel lost that batch; the device now reports B only
	a.resync_down = {{BTN_SOUTH, 0}, {BTN_EAST, 1}};
	send(a, {ev(EV_SYN, SYN_DROPPED, 0), key(BTN_SOUTH, 0), syn()});
	gamepad_evdev_core_poll(&core, &st);
	CHECK(st.buttons == GAMEPAD_BTN_B);
	gamepad_evdev_core_fini(&core);
}

#else

TEST_CASE("the Linux pad reader is built on desktop Linux only")
{
	SUCCEED();
}

#endif
