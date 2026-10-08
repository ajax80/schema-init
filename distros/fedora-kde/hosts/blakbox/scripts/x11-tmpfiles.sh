#!/bin/sh
# schema-init runs no systemd-tmpfiles at boot, so the X11/ICE socket dirs are
# never created root:root 1777 (per /usr/lib/tmpfiles.d/x11.conf). An X server
# REFUSES a /tmp/.X11-unix that lacks the sticky bit, so kwin's XWayland silently
# fails to start and no DISPLAY is exported -> every X11 app (Steam) dies with
# "Unable to open display". /tmp is on the root fs here (not tmpfs) so bad dirs and
# stale sockets persist across reboots. Mirror the tmpfiles "D!" behaviour: empty
# the dirs, set root:root 1777, and clear stale X lock files. Boot oneshot only.
for d in /tmp/.X11-unix /tmp/.ICE-unix /tmp/.XIM-unix /tmp/.font-unix; do
    mkdir -p "$d"
    find "$d" -mindepth 1 -delete 2>/dev/null
    chown root:root "$d" && chmod 1777 "$d"
done
rm -f /tmp/.X[0-9]*-lock
