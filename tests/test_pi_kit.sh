#!/bin/sh
set -u
cd "$(dirname "$0")/.."
D=$PWD
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

mkdir -p "$T/bin" "$T/root/etc"
echo pidev > "$T/root/etc/hostname"
for c in ip hostname swapon sleep; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls"\n%s\n' "$c" "$T" "${SHIM_TAIL:-exit 1}" > "$T/bin/$c"
    chmod +x "$T/bin/$c"
done
# ip must report a default route for timesync, and fail otherwise
cat > "$T/bin/ip" <<EOT
#!/bin/sh
echo "ip \$*" >> "$T/calls"
[ "\$1 \$2 \$3" = "route show default" ] && echo "default via 10.0.0.1 dev eth0"
exit 1
EOT
cat > "$T/bin/sleep" <<EOT
#!/bin/sh
echo "sleep \$*" >> "$T/calls"
EOT
cat > "$T/bin/curl" <<EOT
#!/bin/sh
case "\$*" in
*1.1.1.1*) [ -n "\${T_CF_OK:-}" ] || exit 7; printf 'HTTP/1.1 301\r\nDate: Mon, 02 Nov 2026 03:04:05 GMT\r\n\r\n' ;;
*8.8.8.8*) [ -n "\${T_G_OK:-}" ] || exit 7; printf 'HTTP/1.1 302\r\nDate: Tue, 03 Nov 2026 04:05:06 GMT\r\n\r\n' ;;
esac
EOT
cat > "$T/bin/date" <<EOT
#!/bin/sh
echo "date \$*" >> "$T/calls"
EOT
chmod +x "$T/bin/"*

# early: every shim fails, still exit 0
: > "$T/calls"
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$T/root sh "$D/scripts/schema-pi-early" >/dev/null 2>&1 || fail "early exited non-zero"
grep -qx 'ip link set lo up' "$T/calls" || fail "early: no lo up"
grep -qx 'hostname pidev' "$T/calls" || fail "early: hostname"
grep -qx 'swapon -a' "$T/calls" || fail "early: swapon"

# timesync: 1.1.1.1 down, 8.8.8.8 answers
: > "$T/calls"
PATH=$T/bin:$PATH T_G_OK=1 SCHEMA_PI_ONCE=1 SCHEMA_PI_RESYNC=0 sh "$D/scripts/schema-pi-timesync" >/dev/null 2>&1 || fail "timesync (fallback) exited non-zero"
grep -qx 'date -s Tue, 03 Nov 2026 04:05:06 GMT' "$T/calls" || fail "timesync: fallback date not set: $(cat "$T/calls")"
grep -qx 'sleep 0' "$T/calls" || fail "timesync: no resync sleep"

# primary wins when both answer
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 SCHEMA_PI_ONCE=1 sh "$D/scripts/schema-pi-timesync" >/dev/null 2>&1
grep -qx 'date -s Mon, 02 Nov 2026 03:04:05 GMT' "$T/calls" || fail "timesync: primary date not set"
grep -qx 'sleep 21600' "$T/calls" || fail "timesync: default resync interval"

# both down: retry sleep, no date, exit 0
: > "$T/calls"
PATH=$T/bin:$PATH SCHEMA_PI_ONCE=1 SCHEMA_PI_RETRY=7 sh "$D/scripts/schema-pi-timesync" >/dev/null 2>&1 || fail "timesync (down) exited non-zero"
grep -q '^date' "$T/calls" && fail "timesync set a date with no source"
grep -qx 'sleep 7' "$T/calls" || fail "timesync: no retry sleep"

# mounts: the rest of fstab, best effort, network mounts left out; a failure is logged, never fatal
cat > "$T/bin/mount" <<EOT
#!/bin/sh
echo "mount \$*" >> "$T/calls"
[ -n "\${T_MOUNT_FAIL:-}" ] || exit 0
echo "mount: /srv/usb: special device /dev/sda1 does not exist." >&2
exit 32
EOT
chmod +x "$T/bin/mount"
: > "$T/calls"
PATH=$T/bin:$PATH sh "$D/scripts/schema-pi-mounts" > "$T/out" 2>&1 || fail "mounts exited non-zero"
grep -qx 'mount -a -O no_netdev' "$T/calls" || fail "mounts: $(cat "$T/calls")"
[ ! -s "$T/out" ] || fail "mounts: noisy on success: $(cat "$T/out")"
PATH=$T/bin:$PATH T_MOUNT_FAIL=1 sh "$D/scripts/schema-pi-mounts" > "$T/out" 2>&1 || fail "mounts: failed mount exited non-zero"
grep -qx 'pi-mounts: mount: /srv/usb: special device /dev/sda1 does not exist.' "$T/out" || fail "mounts: failure not logged: $(cat "$T/out")"

# every other script parses under a strict POSIX shell
for s in early timesync udev-trigger dbus mounts; do
    [ -f "scripts/schema-pi-$s" ] || fail "scripts/schema-pi-$s missing"
    sh -n "scripts/schema-pi-$s" || fail "schema-pi-$s syntax"
    [ -x "scripts/schema-pi-$s" ] || fail "schema-pi-$s not executable"
done

KIT="pi-early udev udev-trigger dbus pi-mounts pi-timesync journal-sink"
for n in $KIT; do
    f=services/$n.svc.example
    [ -f "$f" ] || fail "$f missing"
    [ "$(sed -n 's/^name=//p' "$f")" = "$n" ] || fail "$f: name mismatch"
    for d in $(sed -n 's/^dep=//p' "$f"); do
        case " $KIT " in *" $d "*) ;; *) fail "$f: dep $d is not a kit file" ;; esac
        [ -f "services/$d.svc.example" ] || fail "$f: dep $d has no file"
    done
    e=$(sed -n 's/^exec=//p' "$f" | cut -d' ' -f1)
    case $e in /usr/local/*) fail "$f: exec under /usr/local" ;; esac
done

deps() { sed -n 's/^dep=//p' "services/$1.svc.example" | tr '\n' ' '; }
[ -z "$(deps pi-early)" ] || fail "pi-early has deps"
[ "$(deps udev)" = "pi-early " ] || fail "udev deps"
[ "$(deps udev-trigger)" = "udev " ] || fail "udev-trigger deps"
[ "$(deps dbus)" = "pi-early " ] || fail "dbus deps"
[ "$(deps journal-sink)" = "pi-early " ] || fail "journal-sink deps"
[ -z "$(deps pi-timesync)" ] || fail "pi-timesync has deps"
[ "$(deps pi-mounts)" = "udev-trigger " ] || fail "pi-mounts deps"
grep -qx 'oneshot=1' services/pi-mounts.svc.example && grep -qx 'needs_root=1' services/pi-mounts.svc.example ||
    fail "pi-mounts: oneshot needs_root"
# a private mount namespace would keep the mounts from the rest of the system
grep -qx 'private_tmp=0' services/pi-mounts.svc.example && grep -qx 'protect_system=0' services/pi-mounts.svc.example &&
    grep -qx 'protect_home=0' services/pi-mounts.svc.example || fail "pi-mounts: sandbox must be off"
grep -qx 'oneshot=0' services/pi-timesync.svc.example || fail "pi-timesync must be long-running"
grep -qx 'ready_path=/run/dbus/system_bus_socket' services/dbus.svc.example || fail "dbus ready_path"
# Debian's system.conf names /run/dbus/pid: one left by a crash makes the restart refuse to start
grep -q '^exec /usr/bin/dbus-daemon --system --nofork --nopidfile ' scripts/schema-pi-dbus || fail "dbus: no --nopidfile"
grep -qx 'exec=/usr/bin/schema-journal-sink' services/journal-sink.svc.example || fail "journal-sink exec"

grep -q schema-import debian/rules || fail "schema-import not packaged"
# every kit exec and the bus-activation helper the importer's overrides run must be in the deb
for f in services/*.svc.example; do
    e=$(sed -n 's|^exec=/usr/libexec/schema-init/||p' "$f")
    [ -z "$e" ] || grep -q "scripts/$e " debian/schema-init.install || fail "$e not in debian/schema-init.install"
done
grep -q 'scripts/schema-bus-activate ' debian/schema-init.install || fail "schema-bus-activate not packaged"
grep -q 'libexec/schema-init/schema-bus-activate' distros/fedora-installer/migrate/schema-import.py ||
    fail "importer no longer execs schema-bus-activate: recheck the deb"
grep -q '^Recommends:.*libglib2.0-bin' debian/control || fail "gdbus (libglib2.0-bin) not recommended"
grep -q '^Recommends:.*curl' debian/control || fail "curl not recommended"
grep -qx 'usr/libexec/schema-init/schema-import usr/bin/schema-import' debian/schema-init.links || fail "schema-import symlink"

echo "test_pi_kit: PASS"
