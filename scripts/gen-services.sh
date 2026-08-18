#!/bin/sh
# gen-services.sh — generate schema-init .svc stubs from a machine's ENABLED systemd units.
#
# Run this while the machine is STILL on systemd (before migrating). It reads every
# enabled *.service unit, extracts its command line, type, and user, and emits a
# .svc stub so the box comes up under schema-init running what it ran before —
# instead of a bare, generic service set a stranger has to rebuild by hand.
#
# Units schema-init provides or reclaims itself (systemd-*, getty, logind, udevd,
# journald, resolved, ...) are skipped. Dependency ORDERING is not translated —
# schema-init deps reference schema service names, and a dangling dep can hang
# boot; add `dep=` yourself for anything order-sensitive (most commonly `dep=dbus`
# or `dep=mount-fstab`). Each stub is critical=0 so a bad import never wedges boot.
#
# Usage:
#   scripts/gen-services.sh              preview what would be imported (default)
#   scripts/gen-services.sh -o DIR       write DIR/services/<name>.svc for each unit
#   scripts/gen-services.sh --all        include units normally skipped (review first)

set -eu

OUTDIR=""
INCLUDE_ALL=0

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUTDIR="${2:?-o needs a dir}"; shift 2 ;;
        --all) INCLUDE_ALL=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown arg: $1" >&2; usage 1 ;;
    esac
done

command -v systemctl >/dev/null 2>&1 || {
    echo "systemctl not found — run gen-services.sh on the machine while it is still" >&2
    echo "running systemd (before migrating to schema-init)." >&2
    exit 1
}

SKIP='^(systemd-|getty@|serial-getty@|autovt@|user@|user-runtime-dir@|dbus-broker|emergency|rescue|halt|reboot|poweroff|kexec|systemd$)'

svc_name() { printf '%s' "${1%.service}"; }

[ -n "$OUTDIR" ] && mkdir -p "$OUTDIR/services"

n_ok=0; n_skip=0; skipped=""
units=$(systemctl list-unit-files --state=enabled --type=service --no-legend 2>/dev/null | awk '{print $1}')

for u in $units; do
    case "$u" in
        *@.service) n_skip=$((n_skip+1)); skipped="$skipped $u(template)"; continue ;;
    esac
    base=$(svc_name "$u")
    if [ "$INCLUDE_ALL" -eq 0 ] && printf '%s' "$base" | grep -qE "$SKIP"; then
        n_skip=$((n_skip+1)); skipped="$skipped $base"; continue
    fi

    props=$(systemctl show "$u" --property=Type,ExecStart,User,RemainAfterExit 2>/dev/null || true)
    exec_line=$(printf '%s\n' "$props" | sed -n 's/^ExecStart=.*argv\[\]=\([^;]*\);.*/\1/p' | head -1)
    exec_line=$(printf '%s' "$exec_line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    [ -n "$exec_line" ] || { n_skip=$((n_skip+1)); skipped="$skipped $base(no-ExecStart)"; continue; }

    bin=$(printf '%s' "$exec_line" | awk '{print $1}')
    rest=$(printf '%s' "$exec_line" | cut -s -d' ' -f2-)
    type=$(printf '%s\n' "$props" | sed -n 's/^Type=//p' | head -1)
    user=$(printf '%s\n' "$props" | sed -n 's/^User=//p' | head -1)

    case "$type" in oneshot) oneshot=1 ;; *) oneshot=0 ;; esac

    stub=$(
        printf 'name=%s\n' "$base"
        printf 'exec=%s\n' "$bin"
        for a in $rest; do printf 'args=%s\n' "$a"; done
        printf 'oneshot=%s\n' "$oneshot"
        if [ -n "$user" ] && [ "$user" != root ]; then
            printf 'user=%s\n' "$user"
        else
            printf 'needs_root=1\n'
        fi
        printf 'critical=0\n'
    )

    if [ -n "$OUTDIR" ]; then
        printf '%s\n' "$stub" > "$OUTDIR/services/$base.svc"
    else
        echo "--- $base.svc ---"
        printf '%s\n' "$stub"
    fi
    n_ok=$((n_ok+1))
done

echo "" >&2
echo "gen-services: $n_ok stub(s) generated, $n_skip skipped." >&2
[ -z "$skipped" ] || echo "  skipped:$skipped" >&2
echo "" >&2
echo "NOTE: dependency ordering is NOT captured. Add dep= to any stub that must" >&2
echo "  start after another (commonly dep=dbus, dep=mount-fstab). Review every" >&2
echo "  exec/args line — \$OPTIONS-style env expansions in a unit's ExecStart do" >&2
echo "  not resolve here and must be filled in by hand." >&2
if [ -n "$OUTDIR" ]; then
    echo "" >&2
    echo "wrote $n_ok file(s) to $OUTDIR/services/ — install with:" >&2
    echo "  install -m644 $OUTDIR/services/*.svc /etc/schema-init/services/" >&2
fi
