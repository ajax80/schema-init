#!/bin/sh
# The 11-18 regression against the real trees: v0.4.27 (no syscall_deny) as the
# baseline, 434dcac (syscall_deny following the hardening default) as the next
# boot, the same .svc set, hardening-default on. Neither tree has --check, so this
# branch's reporter (check.c, svc_serial.c, karg.c, sha256.c + the --check entry)
# is bolted onto each; the old tree's own service.c/engine.c do the parsing and
# defaulting, which is what is being measured. Builds two trees: not in `make test`.
# usage: test_verify_regression.sh DIFF_DRIVER   (tests/verify_regress_diff.c)
set -eu
cd "$(dirname "$0")/.."
DRV=$1
REPO=$(pwd)
T=$(mktemp -d)
WTS=""
cleanup() {
    for w in $WTS; do git -C "$REPO" worktree remove --force "$w" 2>/dev/null || true; done
    git -C "$REPO" worktree prune
    rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
fail() { echo "FAIL verify_regression: $*"; [ -f "$T/diff" ] && sed 's/^/  | /' "$T/diff"; exit 1; }
need() { grep -q "$1" "$2" || fail "bolt-on: '$1' not in $2 (old tree changed shape)"; }

# Fixture: plain hand-written units of the kind that broke blakbox, plus one that
# opts out explicitly. Made here: git keeps no modes and the loader refuses g/o+w.
umask 022
R=$T/root
S=$R/etc/schema-init/services
mkdir -p "$S"
chmod 0755 "$T"
printf 'on\n' > "$R/etc/schema-init/hardening-default"
printf 'name=mount-home\nexec=/usr/bin/mount\nargs=/home\noneshot=1\n' > "$S/mount-home.svc"
printf 'name=getty-tty1\nexec=/sbin/agetty\nargs=tty1\nargs=linux\n' > "$S/getty-tty1.svc"
printf 'name=sshd\nexec=/usr/sbin/sshd\nargs=-D\ndep=mount-home\n' > "$S/sshd.svc"
printf 'name=logger\nexec=/usr/bin/logger-daemon\n' > "$S/logger.svc"
printf 'name=legacy-zero\nexec=/usr/bin/legacy\nsyscall_deny=0\n' > "$S/legacy-zero.svc"
DEFAULTED="getty-tty1 logger mount-home sshd"
CMDLINE="root=UUID=1118 ro init=/sbin/schema-init"

# the new reporter on the old tree; each edit checked, so a changed tree fails loudly
bolt() {
    W=$1
    for f in check.c check.h karg.c karg.h pi_target.h pi_bootguard.h svc_serial.c svc_serial.h sha256.c sha256.h; do
        cp "$REPO/$f" "$W/$f"
    done
    need '^static int          grp_count' "$W/engine.c"
    printf '\nconst group_t *engine_groups(int *n) {\n    *n = grp_count;\n    return groups;\n}\n' >> "$W/engine.c"
    need '^int engine_load(const char \*svc_dir);' "$W/engine.h"
    sed -i 's/^int engine_load(const char \*svc_dir);/&\nconst group_t *engine_groups(int *n);/' "$W/engine.h"
    # the two key tables the serializer reads, made extern as this branch did (same contents)
    need '^static const char \*const exec_dir_key\[EXEC_DIR_KINDS\]' "$W/service.c"
    need '^static const struct { const char \*key; int res; } rlim_keys\[\]' "$W/service.c"
    sed -i -e 's/\bexec_dir_key\b/service_exec_dir_key/g' \
        -e 's/^static const char \*const service_exec_dir_key/const char *const service_exec_dir_key/' \
        -e 's/^static const struct { const char \*key; int res; } rlim_keys\[\]/const struct service_rlim_key service_rlim_keys[]/' \
        -e 's/\brlim_keys\b/service_rlim_keys/g' "$W/service.c"
    # sc_nrs (sc.hash's numbering), taken verbatim from this branch's seccomp.c
    need '^int sc_count(const sc_spec_t \*s);' "$W/seccomp.h"
    grep -q '^int sc_nrs' "$REPO/seccomp.c" || fail "no sc_nrs in this branch's seccomp.c"
    sed -n '/^static int nr_cmp(/,/^}/p;/^int sc_nrs(/,/^}/p' "$REPO/seccomp.c" >> "$W/seccomp.c"
    sed -i 's/^int sc_count(const sc_spec_t \*s);/&\nint sc_nrs(const uint64_t *set, int *out);/' "$W/seccomp.h"
    need '^enum { EXEC_DIR_RUNTIME' "$W/service.h"
    sed -i '/^enum { EXEC_DIR_RUNTIME/a\
extern const char *const service_exec_dir_key[EXEC_DIR_KINDS];\
#define SERVICE_RLIM_KEYS 16\
struct service_rlim_key { const char *key; int res; };\
extern const struct service_rlim_key service_rlim_keys[];' "$W/service.h"
    # v0.4.27 has no syscall_deny: an empty, never-set field reads as "-" (nothing
    # denied), which is exactly what that parser enforced
    if ! grep -q 'sc_deny\[400\]' "$W/service.h"; then
        need '^    sc_spec_t        sc; ' "$W/service.h"
        need '^#define HARD_PH  0x8' "$W/service.h"
        sed -i -e '/^    sc_spec_t        sc; /a\
    char             sc_deny[400];' -e '/^#define HARD_PH  0x8/a\
#define HARD_SD  0x10' "$W/service.h"
    fi
    need '^#include "svc_dropins.h"' "$W/init.c"
    need '^int main(int argc, char \*\*argv) {' "$W/init.c"
    # --check --root R --cmdline C only; nothing runs before this branch's dispatch off PID 1
    sed -i -e 's/^#include "svc_dropins.h"/&\n#include "check.h"/' \
        -e 's/^int main(int argc, char \*\*argv) {/&\n    if (getpid() != 1 \&\& argc == 6 \&\& !strcmp(argv[1], "--check") \&\& !strcmp(argv[2], "--root") \&\& !strcmp(argv[4], "--cmdline")) return check_main(argv[3], argv[5], NULL);/' \
        "$W/init.c"
}

# one tree at a time: two parallel builds of the whole service stack are heavy
run_tree() {
    tag=$1 ref=$2
    W=$T/wt-$tag
    git -C "$REPO" worktree add -q --detach "$W" "$ref" || fail "worktree add $ref"
    WTS="$WTS $W"
    bolt "$W"
    objs="init.o check.o karg.o engine.o engine_conf.o schema.o service.o watch.o seccomp.o group.o caps.o ns.o landlock.o svc_serial.o sha256.o"
    # shellcheck disable=SC2086
    make -s -C "$W" $objs > "$T/build-$tag.log" 2>&1 || { sed 's/^/  | /' "$T/build-$tag.log"; fail "$ref: build"; }
    (cd "$W" && ${CC:-gcc} -o schema-init $objs -lrt) > "$T/link-$tag.log" 2>&1 ||
        { sed 's/^/  | /' "$T/link-$tag.log"; fail "$ref: link"; }
    rc=0
    "$W/schema-init" --check --root "$R" --cmdline "$CMDLINE" > "$T/$tag.out" 2> "$T/$tag.err" || rc=$?
    [ $rc -eq 0 ] || { sed 's/^/  | /' "$T/$tag.out" "$T/$tag.err"; fail "$ref: --check exit $rc"; }
    [ "$(head -n1 "$T/$tag.out")" = "schema-verify-baseline v1" ] || fail "$ref: not a baseline"
    grep -q '^hardening_default=on$' "$T/$tag.out" || fail "$ref: hardening default not on"
    for s in $DEFAULTED legacy-zero; do
        grep -q "^$s\.exec=" "$T/$tag.out" || fail "$ref: $s not loaded"
    done
    echo "built $ref and ran --check"
}

run_tree base v0.4.27
run_tree next 434dcac

# the trees' own words for what each boot would enforce
for s in $DEFAULTED legacy-zero; do
    grep -q "^$s\.syscall_deny=-$" "$T/base.out" || fail "v0.4.27: $s.syscall_deny not '-'"
done
for s in $DEFAULTED; do
    grep -q "^$s\.syscall_deny=default	src=default$" "$T/next.out" || fail "434dcac: $s.syscall_deny not defaulted"
done
grep -q '^legacy-zero\.syscall_deny=0	src=explicit$' "$T/next.out" || fail "434dcac: legacy-zero not explicit 0"

"$DRV" "$T/base.out" "$T/next.out" > "$T/diff" || fail "diff driver"
sed 's/^/  diff: /' "$T/diff"
grep -q '^FAIL ' "$T/diff" && fail "a FAIL finding"
[ "$(grep -c '^CHANGE syscall_deny .*(default) ' "$T/diff")" = 1 ] || fail "want exactly one defaulted syscall_deny CHANGE"
line=$(grep '^CHANGE syscall_deny .*(default) ' "$T/diff")
n=$(echo "$DEFAULTED" | wc -w)
[ "$line" = "CHANGE syscall_deny  - → default (default)   $n services: $DEFAULTED" ] ||
    fail "defaulted syscall_deny CHANGE: wrong wording or service set"
# The explicit opt-out is in none of what the default changed. v0.4.27 never knew
# the key, so its spelling alone shows as "- → 0 (explicit)": the reporter says
# what the file now means, while enforcement (sc.*) stays the same.
grep 'legacy-zero' "$T/diff" | grep -qv '^CHANGE syscall_deny  - → 0 (explicit)   1 service: legacy-zero$' &&
    fail "legacy-zero changed beyond its explicit spelling"
# the deny merged into each unit's one filter: sc.* moves with it, for the same
# services; anything else changing would be a fixture or bolt-on artefact
for k in sc.count sc.hash sc.mode; do
    grep "^CHANGE $k " "$T/diff" | grep -q "   $n services: $DEFAULTED\$" || fail "$k: not exactly the defaulted services"
done
[ "$(wc -l < "$T/diff")" = 5 ] || fail "unexpected finding"
echo "ok verify_regression: v0.4.27 -> 434dcac reports syscall_deny defaulted on $n services, enforcement unchanged on legacy-zero"
