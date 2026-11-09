#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""gamepad_evdev_uinput.py PROBE: drive the Linux gamepad backend with virtual pads made through /dev/uinput.

The kernel part of the evdev reader (ORDER 478 M2b): the decoder and the device layer are tested without a device
(tests_gamepad_evdev); this is the run on a real kernel device node. It creates pads with /dev/uinput, starts PROBE
(the gamepad_evdev_probe binary), sends events, and checks what the provider printed.

NEVER run it on a machine whose desktop session or games should not see a new pad: a uinput pad is a real input
device to every program of the session (the station's session reads /dev/input too). Run it on the bench machine
(the A10), from a console or an ssh session, as a user that may write /dev/uinput (group input, or root).

    python3 -I tests/gamepad_evdev_uinput.py /path/to/gamepad_evdev_probe        # the run
    python3 -I tests/gamepad_evdev_uinput.py --selftest                           # no device: the structure sizes only

What it must print (every line PASS; exit 0). Pad A is an Xbox-layout pad (BTN_SOUTH.., ABS_X/Y/RX/RY -32768..32767,
ABS_Z/RZ 0..1023, ABS_HAT0X/Y -1..1); pad B is a second pad of the same kind.
  hot plug          pad A created after the probe started   -> STATE connected=1 within 5 s
  buttons           BTN_SOUTH down                           -> buttons has 0x0001 (A); released -> 0
                    BTN_NORTH, BTN_WEST, BTN_START           -> X 0x0004, Y 0x0008, Menu 0x0100
  left stick        ABS_X = 32767                            -> lx 1.000
                    ABS_Y = -32768 (the kernel's "up")       -> ly 1.000 (+Y up) and NAV z falls (forward is -Z)
  right stick       ABS_RX = -32768                          -> rx -1.000 and NAV yaw rises (positive turns left)
  triggers          ABS_Z = 1023, ABS_RZ = 512               -> lt 1.000, rt about 0.500
  d-pad             ABS_HAT0X = -1, ABS_HAT0Y = -1           -> buttons DPAD_LEFT 0x1000 + DPAD_UP 0x0400
  two pads          pad B created, BTN_EAST on B             -> buttons 0x0002 (B speaks: touched last)
                    BTN_SOUTH on A again                     -> buttons 0x0001 (A speaks)
  a pad that goes   pad A destroyed while it speaks          -> B takes over (STATE connected=1)
  none left         pad B destroyed                          -> STATE connected=0 and buttons 0x0000
"""
import fcntl
import os
import select
import struct
import subprocess
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_START = 0x130, 0x131, 0x133, 0x134, 0x13B
BTN_TL, BTN_TR, BTN_SELECT, BTN_MODE, BTN_THUMBL, BTN_THUMBR = 0x136, 0x137, 0x13A, 0x13C, 0x13D, 0x13E
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11

# <linux/uinput.h>
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_DEV_SETUP = 0x405C5503
UI_ABS_SETUP = 0x401C5504
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567

SETUP = struct.Struct("=HHHH80sI")      # struct uinput_setup: input_id, name[80], ff_effects_max
ABS_SETUP = struct.Struct("=HxxiiiiiI") # struct uinput_abs_setup: code, then struct input_absinfo (6 x s32)
EVENT = struct.Struct("@llHHi")        # struct input_event on a 64-bit kernel: timeval, type, code, value


class Pad:
    def __init__(self, name):
        self.name = name
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for k in (BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        for code, lo, hi in ((ABS_X, -32768, 32767), (ABS_Y, -32768, 32767), (ABS_RX, -32768, 32767),
                             (ABS_RY, -32768, 32767), (ABS_Z, 0, 1023), (ABS_RZ, 0, 1023),
                             (ABS_HAT0X, -1, 1), (ABS_HAT0Y, -1, 1)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, ABS_SETUP.pack(code, 0, lo, hi, 0, 0, 0))
        fcntl.ioctl(self.fd, UI_DEV_SETUP, SETUP.pack(0x03, 0x1209, 0xD0A1, 1, name.encode(), 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def send(self, etype, code, value):
        os.write(self.fd, EVENT.pack(0, 0, etype, code, value))

    def sync(self):
        self.send(EV_SYN, 0, 0)

    def key(self, code, down):
        self.send(EV_KEY, code, 1 if down else 0)
        self.sync()

    def axis(self, code, value):
        self.send(EV_ABS, code, value)
        self.sync()

    def destroy(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


class Probe:
    def __init__(self, path, seconds):
        self.p = subprocess.Popen([path, str(seconds)], stdout=subprocess.PIPE, text=True, bufsize=1)
        self.state = None
        self.nav = None
        self.lines = []

    def pump(self, wait):
        """Read the probe's lines for `wait` seconds."""
        end = time.time() + wait
        while time.time() < end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.05)
            if not r:
                continue
            line = self.p.stdout.readline()
            if not line:
                return
            self.lines.append(line.rstrip())
            if line.startswith("STATE "):
                self.state = dict(kv.split("=") for kv in line.split()[1:])
            elif line.startswith("NAV"):
                self.nav = dict(kv.split("=") for kv in line.split()[1:])


results = []


def check(label, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + label + (("  (" + detail + ")") if detail else ""))


def buttons(pr):
    return int(pr.state["buttons"], 16) if pr.state else None


def selftest():
    assert SETUP.size == 0x5C, SETUP.size
    assert ABS_SETUP.size == 0x1C, ABS_SETUP.size
    assert EVENT.size == 24, EVENT.size
    print("selftest: uinput_setup 92, uinput_abs_setup 28, input_event 24 bytes")
    return 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    selftest()
    if not os.access("/dev/uinput", os.W_OK):
        print("SKIP /dev/uinput is not writable by this user (group input, or root): nothing was created")
        return 77
    pr = Probe(sys.argv[1], 40)
    pr.pump(1.0)
    check("the probe starts the evdev backend", any(l.startswith("BACKEND evdev") for l in pr.lines), "; ".join(pr.lines[:2]))
    check("no pad yet: connected=0", pr.state is not None and pr.state["connected"] == "0")
    a = Pad("DXR test pad A")
    t0 = time.time()
    while time.time() - t0 < 5 and not (pr.state and pr.state["connected"] == "1"):
        pr.pump(0.2)
    check("hot plug: pad A found within 5 s", pr.state is not None and pr.state["connected"] == "1", f"{time.time() - t0:.1f} s")
    a.key(BTN_SOUTH, True); pr.pump(0.4)
    check("BTN_SOUTH -> A (0x0001)", buttons(pr) == 0x0001, str(pr.state))
    a.key(BTN_SOUTH, False); pr.pump(0.4)
    check("released -> no buttons", buttons(pr) == 0)
    a.key(BTN_NORTH, True); a.key(BTN_WEST, True); a.key(BTN_START, True); pr.pump(0.4)
    check("BTN_NORTH, BTN_WEST, BTN_START -> X, Y, Menu (0x010C)", buttons(pr) == 0x010C, hex(buttons(pr) or 0))
    for k in (BTN_NORTH, BTN_WEST, BTN_START):
        a.key(k, False)
    z0 = float(pr.nav["z"]) if pr.nav else 0.0
    a.axis(ABS_X, 32767); a.axis(ABS_Y, -32768); pr.pump(1.0)
    check("ABS_X max -> lx 1.000", pr.state and abs(float(pr.state["lx"]) - 1.0) < 0.01, str(pr.state))
    check("ABS_Y min (the kernel's up) -> ly 1.000", pr.state and abs(float(pr.state["ly"]) - 1.0) < 0.01)
    check("the stick up moves the rig forward (NAV z falls)", pr.nav and float(pr.nav["z"]) < z0 - 0.1, f"z {z0} -> {pr.nav['z'] if pr.nav else None}")
    a.axis(ABS_X, 0); a.axis(ABS_Y, 0)
    yaw0 = float(pr.nav["yaw"]) if pr.nav else 0.0
    a.axis(ABS_RX, -32768); pr.pump(1.0)
    check("ABS_RX min -> rx -1.000", pr.state and abs(float(pr.state["rx"]) + 1.0) < 0.01)
    check("the right stick left turns the rig left (NAV yaw rises)", pr.nav and float(pr.nav["yaw"]) > yaw0 + 5.0, f"yaw {yaw0} -> {pr.nav['yaw'] if pr.nav else None}")
    a.axis(ABS_RX, 0)
    a.axis(ABS_Z, 1023); a.axis(ABS_RZ, 512); pr.pump(0.5)
    check("triggers: lt 1.000, rt about 0.500", pr.state and abs(float(pr.state["lt"]) - 1.0) < 0.01 and abs(float(pr.state["rt"]) - 512 / 1023) < 0.01, str(pr.state))
    a.axis(ABS_Z, 0); a.axis(ABS_RZ, 0)
    a.axis(ABS_HAT0X, -1); a.axis(ABS_HAT0Y, -1); pr.pump(0.5)
    check("d-pad hat left + up -> 0x1400", buttons(pr) == 0x1400, hex(buttons(pr) or 0))
    a.axis(ABS_HAT0X, 0); a.axis(ABS_HAT0Y, 0); pr.pump(0.3)
    b = Pad("DXR test pad B")
    pr.pump(2.0)  # found by the next scan
    b.key(BTN_EAST, True); pr.pump(0.5)
    check("two pads: B touched last speaks (0x0002)", buttons(pr) == 0x0002, hex(buttons(pr) or 0))
    b.key(BTN_EAST, False)
    a.key(BTN_SOUTH, True); pr.pump(0.5)
    check("A touched again speaks (0x0001)", buttons(pr) == 0x0001, hex(buttons(pr) or 0))
    a.destroy(); pr.pump(1.5)
    b.key(BTN_EAST, True); pr.pump(0.5)
    check("pad A destroyed: B takes over", pr.state and pr.state["connected"] == "1" and buttons(pr) == 0x0002, str(pr.state))
    b.destroy(); pr.pump(2.0)
    check("none left: connected=0, no buttons", pr.state and pr.state["connected"] == "0" and buttons(pr) == 0, str(pr.state))
    pr.p.terminate()
    print("ALL PASS" if all(results) else "SOME FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
