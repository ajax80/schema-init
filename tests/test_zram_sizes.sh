#!/bin/sh
# audit 2026-11-12, 2.2: zram disksize min(2 x RAM, 8G), mem_limit RAM/2, env overrides win
set -e
cd "$(dirname "$0")/.."
fail=0
check() {   # script ram_kb expected [env...]
    s=$1 r=$2 want=$3; shift 3
    got=$(env SCHEMA_ZRAM_SIZES_ONLY=1 SCHEMA_ZRAM_RAM_KB="$r" "$@" sh "$s")
    if [ "$got" = "$want" ]; then echo "  ok  $s $r $*"; else echo "  FAIL $s $r $*: got '$got' want '$want'"; fail=1; fi
}
for s in scripts/zram-swap.sh distros/fedora-installer/rail/scripts/schema-zram-start.sh; do
    check $s 1000000 "disksize=2048000000 mem_limit=512000000"
    check $s 4194304 "disksize=8589934592 mem_limit=2147483648"
    check $s 16000000 "disksize=8589934592 mem_limit=8192000000"
    check $s 1000000 "disksize=1G mem_limit=256M" SCHEMA_ZRAM_DISKSIZE=1G SCHEMA_ZRAM_MEM_LIMIT=256M
done
exit $fail
