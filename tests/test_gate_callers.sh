#!/bin/sh
# Unattended reboots must skip the verify gate (nobody is there to read a refusal);
# what a person's reboot goes through must stay gated (the systemctl shim: test_systemctl_shim.c).
cd "$(dirname "$0")/.." || exit 1
rc=0
fail() { echo "FAIL: $*"; rc=1; }
CALL='schema-ctl"?\)? +(reboot|poweroff|reexec)|"\$CTL" +(reboot|poweroff|reexec)'

for f in scripts/schema-pi-deadline scripts/schema-dbus-flip-healthcheck.sh \
         distros/fedora-installer/schema-udev-flip-healthcheck.sh distros/fedora-installer/schema-flip-apply.sh; do
    calls=$(grep -E "$CALL" "$f" | grep -v '^ *#')
    [ -n "$calls" ] || fail "$f: no schema-ctl reboot/poweroff/reexec call found"
    echo "$calls" | grep -vE -- "($CALL) --force" | grep -q . && fail "$f: call without --force: $calls"
done

grep -E '^[^#]*schema-ctl' scripts/schema-pi-halt | grep -q -- '--force' && fail "schema-pi-halt carries --force (a person's reboot must stay gated)"

[ $rc = 0 ] && echo "gate_callers: ok"
exit $rc
