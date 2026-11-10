#!/bin/sh
# #488: reboot/poweroff/halt/shutdown on a Pi are systemd-sysv's systemctl links.
# The deb diverts them to an init-aware wrapper (scripts/schema-pi-halt).
set -u
cd "$(dirname "$0")/.."
HALT=$PWD/scripts/schema-pi-halt
POSTINST=$PWD/debian/schema-init.postinst
PRERM=$PWD/debian/schema-init.prerm
T=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

mkdir "$T/bin" "$T/orig" "$T/w"
printf '#!/bin/sh\necho "$*" > "%s/ctl.log"\n' "$T" > "$T/bin/schema-ctl"
for n in reboot poweroff halt shutdown; do
    printf '#!/bin/sh\necho "%s $*" > "%s/orig.log"\n' "$n" "$T" > "$T/orig/$n"
    ln -s "$HALT" "$T/w/$n"
done
chmod +x "$T/bin/schema-ctl" "$T/orig/"*

# $1 = PID 1's comm, $2 = invoked name, rest = args
w() {
    echo "$1" > "$T/comm"; n=$2; shift 2
    rm -f "$T/ctl.log" "$T/orig.log"
    PATH=$T/bin:$PATH SCHEMA_PROC1=$T/comm SCHEMA_PI_SYSTEMD=$T/orig "$T/w/$n" "$@" > "$T/out" 2>&1
}
ctl() { [ "$(cat "$T/ctl.log" 2>/dev/null)" = "$1" ]; }
none() { [ ! -e "$T/ctl.log" ] && [ ! -e "$T/orig.log" ]; }

# under systemd (incl. the Pi's fallback boot): systemd's own, same name and args
w systemd reboot -f || fail "systemd reboot exit"
[ "$(cat "$T/orig.log")" = "reboot -f" ] || fail "systemd reboot: $(cat "$T/orig.log" 2>/dev/null)"
[ ! -e "$T/ctl.log" ] || fail "schema-ctl under systemd"
w systemd shutdown -r +5 hi && [ "$(cat "$T/orig.log")" = "shutdown -r +5 hi" ] || fail "systemd shutdown passthrough"

S=schema-init
w $S reboot && ctl reboot || fail "reboot"
w $S reboot -f && ctl reboot || fail "reboot -f"
w $S poweroff && ctl poweroff || fail "poweroff"
w $S halt && ctl poweroff || fail "halt (no halt verb: powers off)"
w $S halt --reboot && ctl reboot || fail "halt --reboot"
w $S reboot -p && ctl poweroff || fail "reboot -p"
w $S shutdown && ctl poweroff || fail "bare shutdown"
w $S shutdown now && ctl poweroff || fail "shutdown now"
w $S shutdown -h now && ctl poweroff || fail "shutdown -h now"
w $S shutdown -P +0 && ctl poweroff || fail "shutdown -P +0"
w $S shutdown -r now && ctl reboot || fail "shutdown -r now"
w $S shutdown --reboot now && ctl reboot || fail "shutdown --reboot now"
w $S reboot --help && [ -e "$T/orig.log" ] || fail "--help goes to systemd's text"
echo ok wrapper

# refused, never an immediate power-off the caller didn't ask for
for a in "-c" "-k" "+5" "-r 10:00" "-r now bye"; do
    # shellcheck disable=SC2086
    w $S shutdown $a && fail "shutdown $a accepted"
    none || fail "shutdown $a acted"
    grep -q "schema-init" "$T/out" || fail "shutdown $a: no reason given"
done
echo ok refusals

# maintainer scripts: fake dpkg-divert does the --rename moves for real in $T/sbin
mkdir "$T/sbin" "$T/dpkg"
cat > "$T/dpkg/dpkg-divert" <<EOF
#!/bin/sh
echo "\$*" >> "$T/divert.log"
mode=; to=; for a; do case \$a in --add) mode=add;; --remove) mode=rm;; --divert) next=to;; *) [ "\${next:-}" = to ] && { to=\$a; next=; }; f=\$a;; esac; done
# like the real one: adding a diversion that already exists renames nothing
[ "\$mode" = add ] && grep -qx "\$f" "$T/diverted" 2>/dev/null && exit 0
[ "\$mode" = add ] && echo "\$f" >> "$T/diverted"
[ "\$mode" = rm ] && sed -i "\\|^\$f\\\$|d" "$T/diverted"
[ "\$mode" = add ] && { [ -e "\$f" ] || [ -L "\$f" ]; } && mv "\$f" "\$to"
[ "\$mode" = rm ] && { [ -e "\$f.systemd" ] || [ -L "\$f.systemd" ]; } && mv "\$f.systemd" "\$f"
exit 0
EOF
# owned by systemd-sysv unless named in $T/unowned
printf '#!/bin/sh\n[ "$1" = -S ] || exit 1\ngrep -qx "$(basename "$2")" "%s/unowned" 2>/dev/null && exit 1\necho "systemd-sysv: $2"\n' "$T" > "$T/dpkg/dpkg-query"
printf '#!/bin/sh\nexit 0\n' > "$T/dpkg/dpkg-trigger"
chmod +x "$T/dpkg/"*
: > "$T/unowned"
for n in reboot poweroff halt shutdown; do ln -s ../bin/systemctl "$T/sbin/$n"; done
mi() { s=$1; shift; PATH=$T/dpkg:$T/bin:$PATH SCHEMA_SBIN=$T/sbin sh "$s" "$@" > "$T/out" 2>&1; }

mi "$POSTINST" configure || fail "postinst configure exit: $(cat "$T/out")"
for n in reboot poweroff halt shutdown; do
    [ "$(readlink "$T/sbin/$n")" = /usr/libexec/schema-init/schema-pi-halt ] || fail "$n not the wrapper: $(readlink "$T/sbin/$n")"
    [ "$(readlink "$T/sbin/$n.systemd")" = ../bin/systemctl ] || fail "$n.systemd not systemd's"
    grep -q -- "--package schema-init --rename --divert $T/sbin/$n.systemd --add $T/sbin/$n" "$T/divert.log" || fail "$n divert args"
done
mi "$POSTINST" configure 0.4.23-1 || fail "upgrade configure exit"
[ "$(readlink "$T/sbin/reboot")" = /usr/libexec/schema-init/schema-pi-halt ] || fail "upgrade lost the wrapper"
[ "$(readlink "$T/sbin/reboot.systemd")" = ../bin/systemctl ] || fail "upgrade clobbered systemd's"

mi "$PRERM" upgrade 0.4.23-1 || fail "prerm upgrade exit"
[ "$(readlink "$T/sbin/reboot")" = /usr/libexec/schema-init/schema-pi-halt ] || fail "prerm upgrade undid the divert"
mi "$PRERM" remove || fail "prerm remove exit"
for n in reboot poweroff halt shutdown; do
    [ "$(readlink "$T/sbin/$n")" = ../bin/systemctl ] || fail "remove did not restore $n: $(readlink "$T/sbin/$n")"
    [ ! -e "$T/sbin/$n.systemd" ] && [ ! -L "$T/sbin/$n.systemd" ] || fail "$n.systemd left behind"
done
echo ok maintainer

# a name no package owns (no systemd-sysv) is left alone
rm -f "$T/divert.log"; echo shutdown > "$T/unowned"
mi "$POSTINST" configure || fail "configure exit with an unowned name"
[ "$(readlink "$T/sbin/shutdown")" = ../bin/systemctl ] || fail "unowned shutdown replaced"
! grep -q "sbin/shutdown" "$T/divert.log" || fail "unowned shutdown diverted"
mi "$PRERM" remove || fail "prerm remove exit (unowned)"
# no dpkg at all (this test's own host): nothing touched, configure still succeeds
rm -f "$T/divert.log"; : > "$T/unowned"
PATH=$T/bin:/usr/bin:/bin SCHEMA_SBIN=$T/sbin sh -c 'command -v dpkg-query >/dev/null' && { echo "ok maintainer-unowned (host has dpkg: skip no-dpkg case)"; exit 0; }
mkdir "$T/nodpkg"; printf '#!/bin/sh\nexit 0\n' > "$T/nodpkg/dpkg-trigger"; chmod +x "$T/nodpkg/dpkg-trigger"
PATH=$T/nodpkg:$T/bin:/usr/bin:/bin SCHEMA_SBIN=$T/sbin sh "$POSTINST" configure > "$T/out" 2>&1 || fail "configure without dpkg-query"
[ "$(readlink "$T/sbin/reboot")" = ../bin/systemctl ] || fail "no-dpkg configure touched reboot"
echo ok maintainer-unowned
