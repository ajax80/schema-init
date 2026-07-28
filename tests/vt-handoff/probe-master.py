#!/usr/bin/env python3
"""Is a freshly-opened DRM fd able to drop somebody else's master?

This is the single assumption the whole re-exec design rests on. If a cold
restart could just re-open /dev/dri/card0 and DROP_MASTER, none of the fd
handoff would be needed. Run it while a compositor holds the node.
"""
import fcntl
import os
import sys

DRM_IOCTL_SET_MASTER = 0x641E
DRM_IOCTL_DROP_MASTER = 0x641F

node = sys.argv[1] if len(sys.argv) > 1 else '/dev/dri/card0'
fd = os.open(node, os.O_RDWR)
print(f"opened {node} fresh as fd={fd}")

for name, req in (('DROP_MASTER', DRM_IOCTL_DROP_MASTER),
                  ('SET_MASTER', DRM_IOCTL_SET_MASTER)):
    try:
        fcntl.ioctl(fd, req, 0)
        print(f"FRESH_{name}=ok")
    except OSError as e:
        print(f"FRESH_{name}=fail errno={e.errno} {e.strerror}")
