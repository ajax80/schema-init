#!/usr/bin/env python3
"""Stand-in for kwin: takes the session and holds the DRM node open forever.

The point is the *holding*. TakeDevice() hands back a dup of the fd the bridge
opened, so bridge and compositor share one open file description and one DRM
master. A compositor that keeps its end open is why killing the bridge does not
release master in the real world -- and therefore why a cold-restarted bridge
can never drop it. Nothing else in the test rig models that.

  ./fake-compositor.py 226:0
"""
import os
import sys
import time

import dbus

BUS = 'org.freedesktop.login1'
PATH = '/org/freedesktop/login1/session/_31'
IFACE = 'org.freedesktop.login1.Session'


def main():
    major, minor = (int(p) for p in sys.argv[1].split(':'))
    bus = dbus.SystemBus()
    iface = dbus.Interface(bus.get_object(BUS, PATH), IFACE)

    iface.TakeControl(False)
    print("TakeControl ok", flush=True)

    fd, paused = iface.TakeDevice(major, minor)
    held = fd.take()
    print(f"pid={os.getpid()} holds {major}:{minor} as fd={held} paused={bool(paused)}",
          flush=True)

    while True:
        time.sleep(3600)


if __name__ == '__main__':
    main()
