#!/bin/sh
set -u
cd "$(dirname "$0")/.."
D=$PWD
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

mkdir -p "$T/bin" "$T/root/etc"
echo pidev > "$T/root/etc/hostname"
for c in ip hostname swapon sleep systemd-tmpfiles; do
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
*208.67.222.222*) [ -n "\${T_G_OK:-}" ] || exit 7; printf 'HTTP/1.1 302\r\nDate: %s\r\n\r\n' "\${T_G_DATE:-Tue, 03 Nov 2026 04:05:06 GMT}" ;;
esac
EOT
# date: -s is logged, -d parses with the real date, +%s is the clock (T_NOW, default the epoch: a Pi with no RTC)
cat > "$T/bin/date" <<EOT
#!/bin/sh
case "\$1" in
-s) echo "date \$*" >> "$T/calls" ;;
-d) exec $(command -v date) -u "\$@" ;;
+%s) echo "\${T_NOW:-0}" ;;
esac
EOT
chmod +x "$T/bin/"*

# early: every shim fails, still exit 0
: > "$T/calls"
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$T/root sh "$D/scripts/schema-pi-early" >/dev/null 2>&1 || fail "early exited non-zero"
grep -qx 'ip link set lo up' "$T/calls" || fail "early: no lo up"
grep -qx 'hostname pidev' "$T/calls" || fail "early: hostname"
grep -qx 'swapon -a' "$T/calls" || fail "early: swapon"
grep -qx 'systemd-tmpfiles --create --remove --boot --exclude-prefix=/dev' "$T/calls" || fail "early: tmpfiles"

# Pi OS's systemd-nologin.conf creates /run/nologin; nothing else removes it
mkdir -p "$T/root/run"; : > "$T/root/run/nologin"
PATH=$T/bin:$PATH SCHEMA_PI_ROOT=$T/root sh "$D/scripts/schema-pi-early" >/dev/null 2>&1
[ ! -e "$T/root/run/nologin" ] || fail "early: /run/nologin left behind"

# timesync runs from a copy: its floor falls back to its own mtime, which a checkout sets to now
TS=$T/schema-pi-timesync
cp "$D/scripts/schema-pi-timesync" "$TS"; touch -d 2020-01-01 "$TS"

# timesync: 1.1.1.1 down, 208.67.222.222 answers
: > "$T/calls"
PATH=$T/bin:$PATH T_G_OK=1 SCHEMA_PI_ONCE=1 SCHEMA_PI_RESYNC=0 sh "$TS" >/dev/null 2>&1 || fail "timesync (fallback) exited non-zero"
grep -qx 'date -s Tue, 03 Nov 2026 04:05:06 GMT' "$T/calls" || fail "timesync: fallback date not set: $(cat "$T/calls")"
grep -qx 'sleep 0' "$T/calls" || fail "timesync: no resync sleep"

# primary wins when both answer and agree
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 T_G_DATE='Mon, 02 Nov 2026 03:04:35 GMT' SCHEMA_PI_ONCE=1 sh "$TS" >/dev/null 2>&1
grep -qx 'date -s Mon, 02 Nov 2026 03:04:05 GMT' "$T/calls" || fail "timesync: primary date not set"
grep -qx 'sleep 21600' "$T/calls" || fail "timesync: default resync interval"

# both down: retry sleep, no date, exit 0
: > "$T/calls"
PATH=$T/bin:$PATH SCHEMA_PI_ONCE=1 SCHEMA_PI_RETRY=7 sh "$TS" >/dev/null 2>&1 || fail "timesync (down) exited non-zero"
grep -q '^date' "$T/calls" && fail "timesync set a date with no source"
grep -qx 'sleep 7' "$T/calls" || fail "timesync: no retry sleep"

# a clock that ran ahead: one source, or two that disagree, cannot set it back
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_NOW=1793700000 SCHEMA_PI_ONCE=1 SCHEMA_PI_RETRY=7 sh "$TS" > "$T/out" 2>&1 || fail "timesync back1: exit"
grep -q '^date' "$T/calls" && fail "timesync: one source set the clock back: $(cat "$T/calls")"
grep -q 'behind the clock, no second source' "$T/out" || fail "timesync back1: not logged: $(cat "$T/out")"
grep -qx 'sleep 21600' "$T/calls" || fail "timesync back1: retried every 30 s: $(cat "$T/calls")"
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 T_NOW=1793700000 SCHEMA_PI_ONCE=1 sh "$TS" > "$T/out" 2>&1
grep -q '^date' "$T/calls" && fail "timesync: two disagreeing sources set the clock back: $(cat "$T/calls")"
# both answer but disagree: neither moves the clock, even forward from boot
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 SCHEMA_PI_ONCE=1 sh "$TS" > "$T/out" 2>&1
grep -q '^date' "$T/calls" && fail "timesync: disagreeing sources moved the clock forward: $(cat "$T/calls")"
grep -q 'second source does not agree' "$T/out" || fail "timesync disagree: not logged: $(cat "$T/out")"
# ...two that agree within 60 s can
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 T_G_DATE='Mon, 02 Nov 2026 03:04:35 GMT' T_NOW=1793700000 SCHEMA_PI_ONCE=1 sh "$TS" > "$T/out" 2>&1
grep -qx 'date -s Mon, 02 Nov 2026 03:04:05 GMT' "$T/calls" || fail "timesync: agreeing sources did not set the clock back: $(cat "$T/calls")"
# ...nor before the build epoch baked in at package build
sed 's/^BUILD_EPOCH=.*/BUILD_EPOCH=1793620000/' "$TS" > "$T/ts-baked"
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 SCHEMA_PI_ONCE=1 sh "$T/ts-baked" > "$T/out" 2>&1 || fail "timesync baked: exit"
[ "$(grep '^date' "$T/calls")" = 'date -s Tue, 03 Nov 2026 04:05:06 GMT' ] || fail "timesync baked: $(cat "$T/calls") $(cat "$T/out")"
# ...nor, unbaked, before the script's own mtime
touch -d '2026-11-02 12:00 UTC' "$TS"
: > "$T/calls"
PATH=$T/bin:$PATH T_CF_OK=1 T_G_OK=1 SCHEMA_PI_ONCE=1 sh "$TS" > "$T/out" 2>&1 || fail "timesync mtime: exit"
[ "$(grep '^date' "$T/calls")" = 'date -s Tue, 03 Nov 2026 04:05:06 GMT' ] || fail "timesync mtime: $(cat "$T/calls") $(cat "$T/out")"
touch -d 2020-01-01 "$TS"
grep -q '^BUILD_EPOCH=$' scripts/schema-pi-timesync || fail "timesync: BUILD_EPOCH must ship unbaked in the source"
grep -q 's/^BUILD_EPOCH=.*/BUILD_EPOCH=$(SOURCE_DATE_EPOCH)/' debian/rules || fail "debian/rules does not bake BUILD_EPOCH"

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
grep -qx 'mount -a -O no_netdev -t nonfs,nfs4,cifs,smb3,smbfs,ceph,glusterfs,fuse.sshfs,9p,davfs' "$T/calls" || fail "mounts: $(cat "$T/calls")"
[ ! -s "$T/out" ] || fail "mounts: noisy on success: $(cat "$T/out")"
PATH=$T/bin:$PATH T_MOUNT_FAIL=1 sh "$D/scripts/schema-pi-mounts" > "$T/out" 2>&1 || fail "mounts: failed mount exited non-zero"
grep -qx 'pi-mounts: mount: /srv/usb: special device /dev/sda1 does not exist.' "$T/out" || fail "mounts: failure not logged: $(cat "$T/out")"

# every other script parses under a strict POSIX shell
for s in early timesync udev-trigger dbus mounts; do
    [ -f "scripts/schema-pi-$s" ] || fail "scripts/schema-pi-$s missing"
    sh -n "scripts/schema-pi-$s" || fail "schema-pi-$s syntax"
    [ -x "scripts/schema-pi-$s" ] || fail "schema-pi-$s not executable"
done

KIT="pi-early udev udev-trigger dbus sysctl pi-mounts pi-timesync journal-sink"
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
# after coldplug: keys of modules udev loads exist, and sysctl.d globs reach interfaces already up
[ "$(deps sysctl)" = "udev-trigger " ] || fail "sysctl deps"
grep -qx 'exec=/usr/libexec/schema-init/schema-sysctl-apply' services/sysctl.svc.example || fail "sysctl exec"
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

# the Pi kit examples are the deb's; the Fedora RPM's services dir must not carry them
line=$(make -s -n install DESTDIR=/nonexistent 2>/dev/null | grep 'schema-init/services/$' | grep '^install -m 0644 ')
[ -n "$line" ] || fail "make install: services line not found"
for f in pi-early pi-mounts pi-timesync pi-boot-firmware pi-bootok pi-deadline udev udev-trigger dbus sysctl; do
    case " $line " in *" services/$f.svc.example "*) fail "make install ships $f.svc.example to the RPM" ;; esac
done
case " $line " in *" services/journal-sink.svc.example "*) ;; *) fail "make install dropped a shared example: $line" ;; esac

echo "test_pi_kit: PASS"
