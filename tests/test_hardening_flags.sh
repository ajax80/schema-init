#!/bin/sh
# Every shipped binary is PIE with full RELRO (audit 2026-11-12, 1.1), and our
# code carries the stack protector and fortify. Dynamic binaries prove the last
# two by importing __stack_chk_fail and a __*_chk; a static binary holds libc's
# own copies either way, so for those the objects after `--` are checked.
# STATIC_PIE=0: the binaries named in STATIC_BINS were linked plain -static (a
# glibc with no static-pie), so they are checked for RELRO only.
# usage: test_hardening_flags.sh BIN... [-- OBJ...]
set -u
bad=0
fail() { echo "FAIL: $*"; bad=1; }
while [ $# -gt 0 ] && [ "$1" != -- ]; do
    b=$1; shift
    readelf -lW "$b" | grep -q GNU_RELRO || fail "$b: no RELRO"
    if [ "${STATIC_PIE:-1}" = 0 ]; then
        case " ${STATIC_BINS:-} " in *" $b "*) continue ;; esac
    fi
    readelf -h "$b" | grep -q 'Type:.*DYN' || fail "$b: not PIE"
    readelf -dW "$b" | grep -Eq 'BIND_NOW|FLAGS_1.*NOW' || fail "$b: no BIND_NOW"
    if readelf -lW "$b" | grep -q INTERP; then
        nm -D "$b" | grep -q ' U __stack_chk_fail' || fail "$b: no stack protector"
        nm -D "$b" | grep -Eq ' U __[a-z_]+_chk(@|$)' || fail "$b: no fortify"
    fi
done
[ "${1:-}" = -- ] && shift
# a small object with no buffer or fortifiable call has neither, so the set
# is checked as a whole
if [ $# -gt 0 ]; then
    nm "$@" 2>/dev/null | grep -q ' U __stack_chk_fail' || fail "objects: no stack protector"
    nm "$@" 2>/dev/null | grep -Eq ' U __[a-z_]+_chk$' || fail "objects: no fortify"
fi
[ $bad = 0 ] && echo "test_hardening_flags: ok"
exit $bad
