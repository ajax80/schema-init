#!/bin/sh
set -u
cd "$(dirname "$0")/.."
[ -x ./schema-init-pi ] || { echo "FAIL: no ./schema-init-pi"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

R=$T/root
mkdir -p "$R/usr/bin" "$R/lib/systemd" "$R/var/lib/schema-init/known-good"
stub() { printf '#!/bin/sh\necho "%s dir=${SCHEMA_SVC_DIR:-} args=$*"\n' "$1" > "$2"; chmod +x "$2"; }
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
cat > "$T/want" <<EOT
schema-init dir= args=a b
schema-init dir= args=a b
known-good dir=$KG args=a b
systemd dir= args=a b
schema-init dir= args=a b
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
printf 'schema-init dir= args=a b\nschema-init dir= args=a b\nsystemd dir= args=a b\n' > "$T/want"
cmp -s "$T/same" "$T/want" || { diff "$T/want" "$T/same"; fail "identical known-good sequence"; }
echo "test_pi_wrapper: ok"
