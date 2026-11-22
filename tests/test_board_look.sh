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

mk() { S=/schema-board-look-$$-$1${2:-}; SHMS="$SHMS $S"; "$T/mk" "$S" "$@" || fail "mk $*"; }
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
echo "$P" | grep -q "evil${ESC}" && fail "plain: name injected an escape"
echo "$P" | grep -q "evil?\[2Jname" || fail "plain: ESC not shown as ?"

# --- Task 2: two columns, overflow, selection ---
mk 68
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=160x45 board) || fail "68 exit"
for n in svc000 svc040 svc067; do echo "$O" | grep -q "$n" || fail "68@160x45 lacks $n"; done
[ "$(echo "$O" | wc -l)" -le 45 ] || fail "68@160x45 taller than 45 lines"
echo "$O" | grep -q "┣━.*┣━" || fail "68@160x45 not two columns"
echo "$O" | grep -q "more" && fail "68@160x45 overflowed"
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=160x45 SCHEMA_BOARD_SEL=50 board) || fail "sel exit"
[ "$(echo "$O" | wc -l)" -le 45 ] || fail "sel: taller than 45 lines"
echo "$O" | grep -q "┣━.*━►.*svc050" || fail "sel 50 not marked in the second column"
[ "$(echo "$O" | grep -c "${ESC}\[1;93m━►")" -eq 1 ] || fail "sel: marker count != 1"
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=80x45 board) || fail "68@80 exit"
echo "$O" | grep -q "┣━.*┣━" && fail "80 cols drew two columns"
echo "$O" | grep -q "… [0-9]* more" || fail "68@80x45 no overflow line"
[ "$(echo "$O" | wc -l)" -le 45 ] || fail "68@80x45 taller than 45 lines"
mk 120
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=160x45 board) || fail "120 exit"
echo "$O" | grep -q "… [0-9]* more" || fail "120@160x45 no overflow line"
[ "$(echo "$O" | wc -l)" -le 45 ] || fail "120@160x45 taller than 45 lines"
echo "$O" | head -1 | grep -q "S C H E M A" || fail "banner not first line"

# --- final review: lines clipped to the screen, footer kept on tall screens ---
vis_max() { sed "s/${ESC}\[[0-9;]*[A-Za-z]//g" | LC_ALL=C tr -d '\200-\277' | LC_ALL=C awk '{ n = length($0); if (n > m) m = n } END { print m + 0 }'; }  # strip UTF-8 continuation bytes: mawk/busybox length() counts bytes
mk 68
for st in font ascii; do
    O=$(SCHEMA_BOARD_STYLE=$st SCHEMA_BOARD_SIZE=80x25 board) || fail "$st 80x25 exit"
    [ "$(echo "$O" | vis_max)" -le 80 ] || fail "$st 80x25: a line is wider than 80 ($(echo "$O" | vis_max))"
    [ "$(echo "$O" | wc -l)" -le 25 ] || fail "$st 80x25 taller than 25"
    O=$(SCHEMA_BOARD_STYLE=$st SCHEMA_BOARD_SIZE=60x20 board) || fail "$st 60x20 exit"
    [ "$(echo "$O" | vis_max)" -le 60 ] || fail "$st 60x20: a line is wider than 60"
done
mk 128
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=80x200 SCHEMA_BOARD_SEL=3 board) || fail "80x200 exit"
echo "$O" | grep -q "━━►" || fail "80x200: footer lost"
echo "$O" | grep -q "j/k" || fail "80x200: key help lost"

# --- restarts past 999 keep the columns; standby is tallied as STANDBY, not EXCISED ---
mk 10 x
O=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=80x60 board) || fail "x exit"
echo "$O" | grep -q "svc003.* 1k " || fail "1234 restarts not shown as 1k"
echo "$O" | grep -q "svc004.* \*\* " || fail "250000 restarts not shown as **"
rows=$(echo "$O" | grep "svc00[0-9]" | sed "s/${ESC}\[[0-9;]*[A-Za-z]//g" | LC_ALL=C tr -d '\200-\277' | LC_ALL=C awk '{ print length($0) }' | sort -u | wc -l)
[ "$rows" -eq 1 ] || fail "rows differ in width with big restart counts"
tally=$(SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=200x60 board | grep "━━►")
echo "$tally" | grep -q "1 STANDBY" || fail "standby not tallied: $tally"
echo "$tally" | grep -q "EXCISED" && fail "standby tallied as EXCISED: $tally"

echo "PASS: board look"
