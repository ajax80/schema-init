#!/bin/sh
# T12: activatable-service inventory / deferral safety net.
#
# The SP1 broker has NO bus activation (org.freedesktop.DBus.StartServiceByName
# is a stub) and schema-init has no systemd activation. So a D-Bus service that
# was previously started on-demand only becomes DARK after cutover UNLESS the
# schema-init rail already launches its daemon eagerly (the daemon owns its name
# before anyone asks). This audit enumerates every activatable service and marks
# which are covered by the rail vs. which would go dark.
#
# Read-only. Coverage signals:
#   1. Name is owned by a schema-init shim (login1/systemd1/... -> schema-logind
#      / schema-systemd1), OR
#   2. the activatable Exec= binary basename matches a rail service exec= (same
#      daemon, just rail-started instead of bus-activated), OR
#   3. Name is org.freedesktop.DBus (the broker itself).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SVCDIR="/usr/share/dbus-1/system-services"
RAIL="$ROOT/services"

# names the shims already own (from scripts/schema-logind.py + schema-systemd1.py)
SHIM_NAMES=" org.freedesktop.login1 org.freedesktop.systemd1 org.freedesktop.hostname1 org.freedesktop.locale1 org.freedesktop.timedate1 org.freedesktop.ConsoleKit org.freedesktop.DBus "

# activatable names whose daemon the rail starts eagerly under a DIFFERENT name
# than the /bin/false activatable stub (so Exec-binary match can't see it).
RAIL_EXTRA_NAMES=" org.freedesktop.Avahi "

# set of rail daemon basenames. The rail wraps daemons in schema-subreaper, so
# the REAL binary is often in args= (e.g. polkitd.svc: exec=schema-subreaper,
# args=/usr/lib/polkit-1/polkitd) — collect basenames from both exec= and args=.
rail_bins="$(grep -hE '^(exec|args)=' "$RAIL"/*.svc 2>/dev/null | sed 's/^[a-z]*=//; s/[[:space:]].*//; s#.*/##' | grep -v '^schema-subreaper$' | sort -u)"

railed="" ; dark_active="" ; dark_systemd="" ; listing=""

for f in "$SVCDIR"/*.service; do
    [ -e "$f" ] || continue
    name="$(sed -n 's/^Name=//p' "$f" | head -1)"
    exec_line="$(sed -n 's/^Exec=//p' "$f" | head -1)"
    sysd="$(sed -n 's/^SystemdService=//p' "$f" | head -1)"
    [ -n "$name" ] || continue
    bin="$(printf '%s' "$exec_line" | sed 's/[[:space:]].*//; s#.*/##')"

    cover=""
    case "$SHIM_NAMES" in *" $name "*) cover="shim" ;; esac
    case "$RAIL_EXTRA_NAMES" in *" $name "*) [ -z "$cover" ] && cover="rail" ;; esac
    if [ -z "$cover" ] && [ -n "$bin" ] && [ "$bin" != "false" ]; then
        if printf '%s\n' "$rail_bins" | grep -qx "$bin"; then cover="rail:$bin"; fi
    fi

    listing="$listing$name	${exec_line:-<none>}	${sysd:-<none>}
"
    if [ -n "$cover" ]; then
        railed="$railed$name	($cover)
"
    elif [ -n "$bin" ] && [ "$bin" != "false" ]; then
        # real Exec= -> stock dbus-daemon direct-activates it TODAY; regresses at cutover
        dark_active="$dark_active$name	Exec=${exec_line}
"
    else
        # Exec=/bin/false -> needs systemd activation, ALREADY dead under schema-init
        dark_systemd="$dark_systemd$name	SystemdService=${sysd:-<none>}
"
    fi
done

total="$(ls "$SVCDIR"/*.service 2>/dev/null | wc -l | tr -d ' ')"
nrail="$(printf '%s' "$railed" | grep -c .)"
nda="$(printf '%s' "$dark_active" | grep -c .)"
nds="$(printf '%s' "$dark_systemd" | grep -c .)"

echo "# activatable-service inventory ($total services)"
echo
echo "## (a) RAILED — daemon already started by the schema-init rail ($nrail)"
printf '%s' "$railed" | sort
echo
echo "## (b1) REGRESSION SET — real Exec=, bus-activatable TODAY under stock"
echo "##      dbus-daemon, goes dark under schema-dbus (no StartServiceByName) ($nda)"
printf '%s' "$dark_active" | sort
echo
echo "## (b2) already dark under schema-init — Exec=/bin/false, needs systemd"
echo "##      activation (no systemd as PID1), so NOT a schema-dbus regression ($nds)"
printf '%s' "$dark_systemd" | sort
echo
echo "## (c) all activatable names + Exec + SystemdService"
printf 'NAME\tEXEC\tSYSTEMD_SERVICE\n'
printf '%s' "$listing" | sort
