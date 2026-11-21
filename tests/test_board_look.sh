#!/bin/sh
# schema-board's styled (rail-look) render, driven through the test-only
# SCHEMA_BOARD_STYLE / SCHEMA_BOARD_SIZE / SCHEMA_BOARD_SEL overrides.
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
/* mk NAME N: N services cycling every state; #1 long name, #2 an ESC name */
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
    f = shm_open(v[1], O_RDWR | O_CREAT, 0644);
    if (f < 0 || write(f, &z, sizeof z) != (ssize_t)sizeof z) return 1;
    return 0;
}
CEOF
gcc -O2 -std=c99 -D_GNU_SOURCE -I. -o "$T/mk" "$T/mk.c" -lrt || fail "mk build"

mk() { S=/schema-board-look-$$-$1; SHMS="$SHMS $S"; "$T/mk" "$S" "$1" || fail "mk $1"; }
board() { SCHEMA_RECORDER_SHM=/schema-nonexistent-$$ SCHEMA_BOARD_SHM=$S ./schema-board --once; }
ESC=$(printf '\033')

# --- Task 1: styled single column ---
mk 10
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=80x60 board) || fail "font exit"
echo "$O" | grep -q "S C H E M A" || fail "no banner: $O"
echo "$O" | grep -q "B O A R D" || fail "no BOARD word"
echo "$O" | grep -q "┣━" || fail "no branch glyph"
echo "$O" | grep -q "${ESC}\[30;45m" || fail "no PERFECT chip"
echo "$O" | grep -q "${ESC}\[30;41m" || fail "no EXCISED chip"
echo "$O" | grep -q "━━►" || fail "no finish line"
echo "$O" | grep -q "a-very-long-service-n…" || fail "long name not cut with …"
echo "$O" | tail -n +2 | grep -q "${ESC}\[2J" && fail "name injected an escape"
echo "$O" | grep -q "evil?\[2Jname" || fail "ESC not shown as ?"

O=$(SCHEMA_BOARD_STYLE=ascii SCHEMA_BOARD_SIZE=80x60 board) || fail "ascii exit"
echo "$O" | grep -q "+-" || fail "ascii lacks +-"
echo "$O" | grep -q "==>" || fail "ascii lacks ==>"
echo "$O" | grep -q "a-very-long-service-n~" || fail "ascii long name not cut with ~"
echo "$O" | LC_ALL=C grep -q '[^ -~[:cntrl:]]' && fail "ascii output has non-ASCII bytes"

P=$(SCHEMA_BOARD_STYLE=plain board) || fail "plain exit"
D=$(board) || fail "default exit"
[ "$P" = "$D" ] || fail "default (non-VT) output is not the plain board"
echo "$D" | grep -q "limp-mode board" || fail "plain board changed"

echo "PASS: board look"
