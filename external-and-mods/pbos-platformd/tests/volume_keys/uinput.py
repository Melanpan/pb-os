#!/usr/bin/env python3
"""Fake input devices for the volume-keys equivalence test (uinput, root).

Reads commands on stdin, one per line:
  create ID NAME KEY[,KEY...]   a device named NAME (+ = space) with these keys
  destroy ID
  press ID CODE | release ID CODE
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT = (1 << 30) | (4 << 16) | (ord("U") << 8) | 100
UI_SET_KEYBIT = (1 << 30) | (4 << 16) | (ord("U") << 8) | 101
UI_DEV_CREATE = (ord("U") << 8) | 1
UI_DEV_DESTROY = (ord("U") << 8) | 2
EV_SYN, EV_KEY = 0, 1

devs = {}


def emit(fd, etype, code, value):
    t = time.time()
    os.write(fd, struct.pack("llHHi", int(t), int((t % 1) * 1e6), etype, code, value))


for line in sys.stdin:
    a = line.split(maxsplit=3)
    if not a:
        continue
    if a[0] == "create":
        fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        for k in a[3].split(","):
            fcntl.ioctl(fd, UI_SET_KEYBIT, int(k))
        name = a[2].replace("+", " ").encode()[:79]
        # struct uinput_user_dev: name[80], input_id, ff_effects_max, abs[4][64]
        os.write(fd, struct.pack("80sHHHHi", name, 0x06, 1, 1, 1, 0) + bytes(4 * 64 * 4))
        fcntl.ioctl(fd, UI_DEV_CREATE)
        devs[a[1]] = fd
    elif a[0] == "destroy":
        fd = devs.pop(a[1])
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
    elif a[0] in ("press", "release"):
        fd = devs[a[1]]
        emit(fd, EV_KEY, int(a[2]), 1 if a[0] == "press" else 0)
        emit(fd, EV_SYN, 0, 0)
    print("ok", line.strip(), flush=True)
