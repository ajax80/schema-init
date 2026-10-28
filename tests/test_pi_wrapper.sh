#!/bin/sh
set -u
cd "$(dirname "$0")/.."
[ -x ./schema-init-pi ] || { echo "FAIL: no ./schema-init-pi"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

R=$T/root
mkdir -p "$R/usr/bin" "$R/lib/systemd" "$R/var/lib/schema-init/known-good"
stub() { printf '#!/bin/sh\necho "%s dir=${SCHEMA_SVC_DIR:-} argv0=$0 args=$*"\n' "$1" > "$2"; chmod +x "$2"; }
stub schema-init "$R/usr/bin/schema-init"
stub known-good "$R/var/lib/schema-init/known-good/schema-init"
stub systemd "$R/lib/systemd/systemd"

run() {
    rm -f "$R/var/log/schema-init/boot.log"
    SCHEMA_PI_ROOT=$R ./schema-init-pi a b > "$T/stdout" 2>&1
    if [ -s "$R/var/log/schema-init/boot.log" ]; then cat "$R/var/log/schema-init/boot.log"; else cat "$T/stdout"; fi
}

: > "$T/seq"
for n in 1 2 3 4 5; do run >> "$T/seq"; done
KG=$R/var/lib/schema-init/known-good/services
CUR=$R/usr/bin/schema-init
KGB=$R/var/lib/schema-init/known-good/schema-init
SD=$R/lib/systemd/systemd
cat > "$T/want" <<EOT
schema-init dir= argv0=$CUR args=a b
schema-init dir= argv0=$CUR args=a b
known-good dir=$KG argv0=$KGB args=a b
systemd dir= argv0=$SD args=a b
schema-init dir= argv0=$CUR args=a b
EOT
cmp -s "$T/seq" "$T/want" || { diff "$T/want" "$T/seq"; fail "sequence"; }
grep -qx 'attempts=1' "$R/var/lib/schema-init/pi-boot" || fail "state after 5th run"

rm -rf "$R/var"; mkdir -p "$R/var/lib/schema-init/known-good"
stub known-good "$R/var/lib/schema-init/known-good/schema-init"
for n in 1 2 3 4; do run > "$T/o$n"; done
grep -q '^systemd' "$T/o4" || fail "4th run not systemd"
grep -qx 'attempts=0' "$R/var/lib/schema-init/pi-boot" || fail "attempts not reset after systemd"
[ "$(wc -l < "$R/var/log/schema-init/fallback.log")" = 2 ] || fail "fallback.log lines"
grep -Eq '^[0-9]+ fallback=known-good attempts=2$' "$R/var/log/schema-init/fallback.log" || fail "fallback.log format"
grep -Eq '^[0-9]+ fallback=systemd attempts=3$' "$R/var/log/schema-init/fallback.log" || fail "fallback.log format 2"

rm -rf "$R/var"; mkdir -p "$R/var/lib/schema-init/known-good"
cp "$R/usr/bin/schema-init" "$R/var/lib/schema-init/known-good/schema-init"
: > "$T/same"
for n in 1 2 3; do run >> "$T/same"; done
printf 'schema-init dir= argv0=%s args=a b\nschema-init dir= argv0=%s args=a b\nsystemd dir= argv0=%s args=a b\n' "$CUR" "$CUR" "$SD" > "$T/want"
cmp -s "$T/same" "$T/want" || { diff "$T/want" "$T/same"; fail "identical known-good sequence"; }
# known-good is the whole set: a services-only change that fails boot must still try it
SV=$R/etc/schema-init/services
KSV=$R/var/lib/schema-init/known-good/services
svc_case() {
    rm -rf "$R/var" "$SV"; mkdir -p "$KSV" "$SV/net.svc.d" "$KSV/net.svc.d"
    cp "$CUR" "$KGB"
    for d in "$SV" "$KSV"; do
        echo sshd > "$d/sshd.svc"; echo net > "$d/net.svc"; echo dropin > "$d/net.svc.d/a.conf"
    done
    # promote() rewrites these two exec= lines on purpose; that alone is not a different set
    echo exec=/usr/libexec/schema-init/schema-pi-bootok > "$SV/pi-bootok.svc"
    echo exec=/var/lib/schema-init/known-good/schema-pi-bootok > "$KSV/pi-bootok.svc"
    echo exec=/usr/libexec/schema-init/schema-pi-deadline > "$SV/pi-deadline.svc"
    echo exec=/var/lib/schema-init/known-good/schema-pi-deadline > "$KSV/pi-deadline.svc"
    eval "$2"
    for n in 1 2 3; do run > "$T/o$n"; done
    grep -q " argv0=$3 " "$T/o3" || fail "svc_case $1: 3rd boot: $(cat "$T/o3")"
}
svc_case edited 'echo net2 > "$SV/net.svc"' "$KGB"
svc_case added 'echo x > "$SV/new.svc"' "$KGB"
svc_case removed 'rm "$SV/net.svc"' "$KGB"
svc_case dropin 'echo changed > "$SV/net.svc.d/a.conf"' "$KGB"
svc_case same ':' "$SD"
rm -rf "$SV"

rm -rf "$R/var"; mkdir -p "$R/var/lib/schema-init"
SCHEMA_SVC_DIR=$T run > "$T/leak"
grep -q '^schema-init dir= ' "$T/leak" || fail "inherited SCHEMA_SVC_DIR leaked into current"
echo "test_pi_wrapper: ok"
