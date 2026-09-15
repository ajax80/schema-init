#!/bin/sh
# Enable kernel media-change polling on optical drives so tray
# insert/eject emits a change uevent (lets discs automount).
for d in /sys/block/sr*/events_poll_msecs; do
	[ -e "$d" ] && echo 2000 > "$d"
done
exit 0
