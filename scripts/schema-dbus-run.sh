#!/bin/sh
# SP1 cutover launcher for the schema-dbus C broker.
#
# The broker needs the system D-Bus policy as a dissolved file (it does NOT parse
# busconfig XML itself); its built-in default denies all name ownership, which
# would break every service. So dissolve the LIVE busconfig into the broker's
# policy format at boot, then exec the broker on the system bus socket. If the
# dissolve fails for any reason, fall back to stock dbus-daemon so the box still
# boots with a working bus — the cutover is self-healing, never a dead bus.
# The dissolved policy is cached in /var/cache/schema-init and used as is until a
# D-Bus config file or the dissolver is newer than the cache. Every bus client
# waits on this script, so the cached path runs shell builtins only: on a
# spinning disk python3 costs ~0.6 s cold and each coreutils exec tens of ms.
set -u

case "$0" in */*) self_dir=${0%/*} ;; *) self_dir=. ;; esac
self_dir=$(CDPATH= cd -- "$self_dir" && pwd)

find_bin() {
    for p in /usr/local/bin/"$1" /usr/bin/"$1" /sbin/"$1" \
             "$self_dir/../$1" "$self_dir/$1"; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    command -v "$1" 2>/dev/null
}

DISSECT=""
for p in "$self_dir/dissect_policy.py" \
         "$self_dir/../lib/schema-init/dissect_policy.py" \
         /usr/local/lib/schema-init/dissect_policy.py \
         /usr/lib/schema-init/dissect_policy.py \
         "$self_dir/../tools/dbus-learn/dissect_policy.py"; do
    [ -f "$p" ] && { DISSECT="$p"; break; }
done

BROKER=${SCHEMA_DBUS_BROKER:-$(find_bin schema-dbus)}
STOCK=${SCHEMA_DBUS_STOCK:-$(find_bin dbus-daemon)}
POLDIR=${SCHEMA_DBUS_POLDIR:-/run/schema-init}
POL="$POLDIR/dbus.policy"

fallback() {
    echo "schema-dbus-run: $1 — falling back to stock dbus-daemon" >&2
    [ -n "$STOCK" ] || { echo "schema-dbus-run: no dbus-daemon to fall back to — no system bus" >&2; exit 1; }
    if [ -n "${LISTEN_FDS:-}" ]; then
        if command -v python3 >/dev/null 2>&1; then
            exec python3 -c '
import os,sys,socket
p=os.environ.get("NOTIFY_SOCKET","")
if p:
    s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    for n in set(os.environ.get("LISTEN_FDNAMES","").split(":"))&{"listen","state","conn","pfd"}:
        try: s.sendto(("FDSTOREREMOVE=1\nFDNAME="+n).encode(),"\0"+p[1:] if p[0]=="@" else p)
        except OSError: pass
for k in ("LISTEN_FDS","LISTEN_PID","LISTEN_FDNAMES","NOTIFY_SOCKET"): os.environ.pop(k,None)
os.closerange(3, 1 << 20); os.execv(sys.argv[1], sys.argv[1:])' "$STOCK" --system --nofork
        fi
        if command -v perl >/dev/null 2>&1; then
            exec perl -e '
use POSIX (); use Socket;
if (my $p = $ENV{NOTIFY_SOCKET}) {
    $p = "\0" . substr($p, 1) if $p =~ /^@/;
    my %w = map { $_ => 1 } grep { /^(listen|state|conn|pfd)$/ } split /:/, $ENV{LISTEN_FDNAMES} // "";
    if (socket(my $s, AF_UNIX, SOCK_DGRAM, 0)) { send($s, "FDSTOREREMOVE=1\nFDNAME=$_", 0, pack_sockaddr_un($p)) for keys %w }
}
delete @ENV{qw(LISTEN_FDS LISTEN_PID LISTEN_FDNAMES NOTIFY_SOCKET)};
POSIX::close($_) for 3..1048575; exec {$ARGV[0]} @ARGV' "$STOCK" --system --nofork
        fi
        unset LISTEN_FDS LISTEN_PID LISTEN_FDNAMES NOTIFY_SOCKET
        fdd=/proc/self/fd
        [ -d "$fdd" ] || fdd=/dev/fd
        for f in "$fdd"/*; do
            n=${f##*/}
            case $n in [3-9]) eval "exec $n>&-" 2>/dev/null || : ;; esac
        done
    fi
    exec "$STOCK" --system --nofork
}

[ -e "${SCHEMA_DBUS_GATE:-/etc/schema-init/dbus-broker}" ] || fallback "broker not armed"
[ -n "$BROKER" ] || fallback "schema-dbus binary not found"
[ -n "$DISSECT" ] || fallback "dissect_policy.py not found"

CACHE=${SCHEMA_DBUS_CACHE:-/var/cache/schema-init/dbus.policy}

cache_fresh() {
    [ -s "$CACHE" ] || return 1
    for f in /usr/share/dbus-1/system.conf /usr/share/dbus-1/system.d /usr/share/dbus-1/system.d/* \
             /etc/dbus-1 /etc/dbus-1/* /etc/dbus-1/*/* "$DISSECT" "$BROKER"; do
        [ "$f" -nt "$CACHE" ] && return 1
    done
    return 0
}

if cache_fresh; then
    POL=$CACHE
elif mkdir -p "$POLDIR" && : > "$POL.start" && python3 "$DISSECT" /usr/share/dbus-1/system.conf > "$POL.tmp" 2>/dev/null && [ -s "$POL.tmp" ]; then
    mv -f "$POL.tmp" "$POL"
    # stamped with the dissolve's start, so a file changed during it is newer
    mkdir -p "${CACHE%/*}" && cp "$POL" "$CACHE.tmp" 2>/dev/null && touch -r "$POL.start" "$CACHE.tmp" &&
        mv -f "$CACHE.tmp" "$CACHE"
    rm -f "$POL.start"
else
    rm -f "$POL.tmp"
    fallback "policy dissolve failed"
fi

export SCHEMA_DBUS_POLICY="$POL"
exec "$BROKER" --system
