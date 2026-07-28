#!/bin/sh
# switch.sh MODE TARGET_VT
#
# Drives a real kernel VT switch from inside the guest and records what the
# kernel actually did, so a still screen can be told apart from a switch that
# was never requested. sendkey chords are not trustworthy on their own here:
# the kernel refuses to switch to a VT that was never allocated, and that looks
# identical to "the compositor kept the display".
MODE=${1:?usage: switch.sh <mode> <vt>}
TARGET=${2:?usage: switch.sh <mode> <vt>}
OUT=/mnt/out
exec >"$OUT/$MODE-switch.log" 2>&1
set -x

echo "TTYS=$(ls /dev/tty[0-9] 2>/dev/null | tr '\n' ' ')"
echo "VT_BEFORE=$(cat /sys/class/tty/tty0/active)"
echo "FGCONSOLE=$(fgconsole 2>&1)"

# A wedged mediation deadlocks here rather than returning, which is itself the
# answer -- so cap it instead of hanging the run.
timeout 10 chvt "$TARGET"
echo "CHVT_RC=$?"
sleep 2
echo "VT_AFTER=$(cat /sys/class/tty/tty0/active)"
echo SWITCHED > "$OUT/$MODE-switch.ready"
