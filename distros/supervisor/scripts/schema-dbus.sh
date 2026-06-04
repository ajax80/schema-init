#!/bin/sh
mkdir -p /run/dbus
chown messagebus:messagebus /run/dbus
chmod 755 /run/dbus
mkdir -p /var/log/schema-init
exec /usr/bin/dbus-daemon --system --nofork >>/var/log/schema-init/dbus.log 2>&1
