#!/bin/sh
# schema-init sysprep: runtime dirs + cold-plug /dev via systemd-udevd.
mkdir -p /run/systemd/system /run/dbus /run/udev /run/lock /run/user 2>/dev/null
/usr/lib/systemd/systemd-udevd --daemon
udevadm trigger --type=subsystems --action=add
udevadm trigger --type=devices --action=add
udevadm settle --timeout=30
exit 0
