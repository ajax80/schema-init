#!/bin/sh
# schema-board's top view (schema-top --once): fake shm via SCHEMA_BOARD_SHM,
# fake cgroup tree via SCHEMA_BOARD_CGROOT.
set -u
cd "$(dirname "$0")/.."
[ -x ./schema-board ] || { echo "FAIL: no ./schema-board"; exit 1; }
T=$(mktemp -d)
SHMS=
cleanup() { for s in $SHMS; do "$T/mk" rm "$s"; done; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
cat > "$T/mk.c" <<'CEOF'
#include "schema.h"
#include "schema_shm.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* mk NAME N [x]: N services cycling every state; #1 long name, #2 an ESC name;
 * x: #3 and #4 with 4- and 6-digit restarts, #6 (EXCISED) held on standby */
int main(int c, char **v) {
    static const uint8_t st[] = { STATE_PERFECT, STATE_FUNDAMENTAL, STATE_SETTLED, STATE_NEW_PROCESS,
                                  STATE_FRICTION, STATE_DORMANT, STATE_EXCISED, STATE_FULL_TRUST };
    schema_shm_t z;
    int f, i, n;
    if (c > 2 && !strcmp(v[1], "rm")) { shm_unlink(v[2]); return 0; }
    if (c < 3) return 2;
    n = atoi(v[2]);
    memset(&z, 0, sizeof z);
    z.magic = SCHEMA_SHM_MAGIC;
    z.seq = 7;
    z.count = n;
    for (i = 0; i < n; i++) {
        snprintf(z.svc[i].name, sizeof z.svc[i].name, "svc%03d", i);
        z.svc[i].state = st[i % 8];
        z.svc[i].weight = 8;
        z.svc[i].child_pid = 1000 + i;
    }
    if (n > 1) snprintf(z.svc[1].name, 64, "a-very-long-service-name-that-overflows");
    if (n > 2) snprintf(z.svc[2].name, 64, "evil\033[2Jname");
    if (c > 3 && n > 6) {
        z.svc[3].restart_count = 1234;
        z.svc[4].restart_count = 250000;
        z.svc[6].limp = SHM_LIMP_STANDBY;
    }
    f = shm_open(v[1], O_RDWR | O_CREAT, 0644);
    if (f < 0 || write(f, &z, sizeof z) != (ssize_t)sizeof z) return 1;
    return 0;
}
CEOF
gcc -O2 -std=c99 -D_GNU_SOURCE -I. -o "$T/mk" "$T/mk.c" -lrt || fail "mk build"

mk() { S=/schema-top-$$-$1${2:-}; SHMS="$SHMS $S"; "$T/mk" "$S" "$@" || fail "mk $*"; }
ln -sf "$PWD/schema-board" "$T/schema-top"
CG=$T/cg
mkdir -p "$CG/svc000"
echo 52428800 > "$CG/svc000/memory.current"
printf 'usage_usec 0\nuser_usec 0\n' > "$CG/svc000/cpu.stat"
top() { SCHEMA_RECORDER_SHM=/schema-nonexistent-$$ SCHEMA_BOARD_SHM=$S SCHEMA_BOARD_CGROOT=$CG "$T/schema-top" --once; }
ESC=$(printf '\033')
vis_max() { sed "s/${ESC}\[[0-9;]*[A-Za-z]//g" | LC_ALL=C tr -d '\200-\277' | LC_ALL=C awk '{ n = length($0); if (n > m) m = n } END { print m + 0 }'; }

mk 30
# BOARD_SAVE=f: record the board view for this fixture (before a change), then stop
[ -z "${BOARD_SAVE:-}" ] || { SCHEMA_RECORDER_SHM=/schema-nonexistent-$$ SCHEMA_BOARD_SHM=$S ./schema-board --once > "$BOARD_SAVE"; exit 0; }
O=$(SCHEMA_BOARD_SIZE=80x25 top) || fail "exit $?"
echo "$O" | grep -q "sort: CPU" || fail "no sort in header: $O"
echo "$O" | grep -q "30 services" || fail "no service count"
echo "$O" | grep "svc000" | grep -q "50.0M" || fail "svc000 MEM: $(echo "$O" | grep svc000)"
echo "$O" | grep "svc003" | sed "s/${ESC}\[[0-9;]*m//g" | grep -q "%\|M\b" && fail "svc003 (no cgroup) shows numbers"
[ "$(echo "$O" | vis_max)" -le 80 ] || fail "80x25: a line is wider than 80"
O60=$(SCHEMA_BOARD_SIZE=60x20 top)
[ "$(echo "$O60" | vis_max)" -le 60 ] || fail "60x20: a line is wider than 60"
echo "$O" | grep -q "a-very-long-service-n" || fail "long name not cut to 22"
echo "$O" | grep -q "a-very-long-service-name-that" && fail "long name not cut"
echo "$O" | tail -n +2 | grep -q "${ESC}\[2J" && fail "name injected an escape"
echo "$O" | grep -q "evil?\[2Jname" || fail "ESC not shown as ?"
SCHEMA_BOARD_SHM=/schema-nonexistent-$$ "$T/schema-top" --once > "$T/out" 2> "$T/err" && fail "no table: exit 0"
grep -q "schema-init" "$T/err" || fail "no table: message: $(cat "$T/err")"
B1=$(SCHEMA_RECORDER_SHM=/schema-nonexistent-$$ SCHEMA_BOARD_SHM=$S ./schema-board --once)
[ -z "${BOARD_BEFORE:-}" ] || [ "$B1" = "$(cat "$BOARD_BEFORE")" ] || fail "schema-board --once changed"
echo "PASS: board top"
