#!/bin/sh
# deploy-canonical-logind.sh — install the canonical schema-logind.py on THIS
# box, restart the service, and smoke-test the security gate. Run as root.
#
#   blakbox (has the repo):
#     sudo sh ~/projects/schema-init/scripts/deploy-canonical-logind.sh
#
#   Eli (no repo): copy the canonical + this script over first, then run:
#     scp ~/projects/schema-init/scripts/schema-logind.py Eli:/tmp/canon.py
#     scp ~/projects/schema-init/scripts/deploy-canonical-logind.sh Eli:/tmp/
#     ssh -t Eli 'sudo sh /tmp/deploy-canonical-logind.sh /tmp/canon.py'
#
# No reboot needed. But this is NOT a plain service restart when a compositor
# is logged in: a Wayland session DOES take its DRM node through logind
# TakeDevice, and the fd we hand it is the same open file description we later
# DROP_MASTER on. kill+respawn loses those fds for good — the bridge comes back
# unable to drop master, VT mediation is never re-armed, and ctrl-alt-F<n> goes
# silently dead until that session logs out. (Seen for real on blakbox
# 2026-07-27: the board was unreachable from 21:09 until a re-login at 22:54.)
#
# So when the bridge is already running we send SIGHUP and it execv()s itself,
# keeping its pid and every fd. Restart is only for a cold bridge.

CANON="${1:-$(dirname "$0")/schema-logind.py}"
LIVE=/usr/local/bin/schema-logind.py

if [ "$(id -u)" != 0 ]; then echo "run as root: sudo sh $0 [canonical.py]"; exit 1; fi
if [ ! -f "$CANON" ]; then echo "canonical not found: $CANON"; exit 1; fi

# parse-check the new file before touching the live one
if ! python3 -m py_compile "$CANON"; then echo "ABORT: $CANON does not parse"; exit 1; fi

# timestamped backup
TS=$(date +%Y%m%d-%H%M%S)
if [ -f "$LIVE" ]; then cp "$LIVE" "$LIVE.bak-$TS" && echo "backup -> $LIVE.bak-$TS"; fi
REVERT="sudo cp $LIVE.bak-$TS $LIVE && sudo kill -HUP \$(pgrep -f schema-logind.py)"

# install
cp "$CANON" "$LIVE" || { echo "ABORT: install failed"; exit 1; }
echo "installed canonical -> $LIVE ($(wc -l <"$LIVE") lines)"

# reload: SIGHUP re-exec if it is live, restart only if it is not
OLDPID=$(pgrep -f "[s]chema-logind.py" | head -1)
if [ -n "$OLDPID" ]; then
    echo "bridge live at pid $OLDPID — SIGHUP re-exec (fds preserved)"
    kill -HUP "$OLDPID" || { echo "ABORT: SIGHUP failed. REVERT: $REVERT"; exit 1; }
else
    echo "no bridge running — cold start"
    schema-ctl restart schema-logind || { echo "ABORT: restart failed. REVERT: $REVERT"; exit 1; }
fi
i=0
while [ $i -lt 8 ]; do
    if dbus-send --system --print-reply --dest=org.freedesktop.login1 \
         /org/freedesktop/login1 org.freedesktop.login1.Manager.CanReboot >/dev/null 2>&1; then
        echo "login1 back"; break
    fi
    i=$((i+1)); sleep 1
done

# a re-exec that silently became a respawn is the whole bug coming back
FAIL=0
if [ -n "$OLDPID" ]; then
    NEWPID=$(pgrep -f "[s]chema-logind.py" | head -1)
    printf 'pid preserved across re-exec:           '
    if [ "$NEWPID" = "$OLDPID" ]; then echo "PASS ($OLDPID)"
    else echo "FAIL ($OLDPID -> ${NEWPID:-gone}) — fds and VT mediation were LOST"; FAIL=1; fi
    printf 'handoff adopted:                        '
    if tail -50 /var/log/schema-init/schema-logind.log 2>/dev/null | grep -q "adopted handoff"; then
        echo "PASS"
    else echo "WARN (no 'adopted handoff' line — check the log)"; fi
fi

# smoke test
P=/org/freedesktop/login1/session/_31
M=org.freedesktop.login1.Session.TakeDevice

printf 'probe nobody TakeDevice (expect DENY):  '
if sudo -u nobody dbus-send --system --print-reply --dest=org.freedesktop.login1 "$P" "$M" \
     uint32:13 uint32:64 2>&1 | grep -q AccessDenied; then echo "PASS (denied)"
else echo "FAIL (NOT denied!)"; FAIL=1; fi

printf 'probe root   TakeDevice (expect ALLOW): '
if dbus-send --system --print-reply --dest=org.freedesktop.login1 "$P" "$M" \
     uint32:13 uint32:64 2>&1 | grep -qiE "file descriptor|FailedToOpen"; then echo "PASS (allowed)"
else echo "FAIL (denied!)"; FAIL=1; fi

printf 'probe timedate1 Timezone:               '
TZV=$(dbus-send --system --print-reply --dest=org.freedesktop.timedate1 /org/freedesktop/timedate1 \
        org.freedesktop.DBus.Properties.Get string:org.freedesktop.timedate1 string:Timezone 2>&1 \
        | grep -oE 'string "[^"]*"' | head -1)
if [ -n "$TZV" ]; then echo "PASS ($TZV)"; else echo "FAIL (no answer)"; FAIL=1; fi

printf 'session Type (detected):                '
dbus-send --system --print-reply --dest=org.freedesktop.login1 "$P" \
    org.freedesktop.DBus.Properties.Get string:org.freedesktop.login1.Session string:Type 2>&1 \
    | grep -oE 'string "[^"]*"' | head -1

if [ "$FAIL" = 0 ]; then
    echo "RESULT: PASS — canonical deployed + gate verified on $(hostname)"
else
    echo "RESULT: FAIL on $(hostname) — REVERT: $REVERT"
    exit 1
fi
