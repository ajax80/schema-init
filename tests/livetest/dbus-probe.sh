#!/bin/sh
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/dbus-probe"
SRC="$DIR/dbus-probe.c"
CSV="$DIR/boot-logs/dbus-history.csv"
N="${1:-500}"

if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    gcc -O2 -o "$BIN" "$SRC" $(pkg-config --cflags --libs dbus-1)
fi

PID="$(pgrep -x schema-dbus || true)"
if [ -n "$PID" ]; then
    BROKER="schema-dbus"
else
    BROKER="stock-dbus-daemon"
    PID="$(pgrep -x dbus-daemon | head -1)"
fi

set -- $(ps -o etimes=,rss= -p "$PID")
ETIMES="$1"; RSS="$2"
UTIME="$(awk '{print $14}' /proc/$PID/stat)"
STIME="$(awk '{print $15}' /proc/$PID/stat)"
CLK="$(getconf CLK_TCK)"
CPU_S="$(awk -v u="$UTIME" -v s="$STIME" -v k="$CLK" 'BEGIN{printf "%.2f",(u+s)/k}')"
CPU_PCT="$(awk -v c="$CPU_S" -v e="$ETIMES" 'BEGIN{printf "%.4f",(e>0)?(c/e)*100:0}')"

NAMES="$(busctl --system --no-pager call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus ListNames 2>/dev/null | tr ' ' '\n' | grep -c '"' || true)"
CONNS="$(ss -x 2>/dev/null | grep -c system_bus_socket || true)"

LAT="$(timeout 30 "$BIN" "$N")"
MIN="$(echo "$LAT"  | sed -n 's/.*min=\([0-9.]*\).*/\1/p')"
P50="$(echo "$LAT"  | sed -n 's/.*p50=\([0-9.]*\).*/\1/p')"
P90="$(echo "$LAT"  | sed -n 's/.*p90=\([0-9.]*\).*/\1/p')"
P99="$(echo "$LAT"  | sed -n 's/.*p99=\([0-9.]*\).*/\1/p')"
MAX="$(echo "$LAT"  | sed -n 's/.*max=\([0-9.]*\).*/\1/p')"
MEAN="$(echo "$LAT" | sed -n 's/.*mean=\([0-9.]*\).*/\1/p')"

printf 'broker=%s pid=%s rss=%sk lifetime_cpu=%ss elapsed=%ss cpu_pct=%s%% names=%s conns=%s\n' \
    "$BROKER" "$PID" "$RSS" "$CPU_S" "$ETIMES" "$CPU_PCT" "$NAMES" "$CONNS"
printf 'latency_us: %s\n' "$LAT"

if [ ! -f "$CSV" ]; then
    printf 'timestamp,broker,rss_kb,lifetime_cpu_s,elapsed_s,cpu_pct,names,conns,lat_min_us,lat_p50_us,lat_p90_us,lat_p99_us,lat_max_us,lat_mean_us\n' > "$CSV"
fi
printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date +%Y%m%d-%H%M%S)" "$BROKER" "$RSS" "$CPU_S" "$ETIMES" "$CPU_PCT" \
    "$NAMES" "$CONNS" "$MIN" "$P50" "$P90" "$P99" "$MAX" "$MEAN" >> "$CSV"
printf 'appended -> %s\n' "$CSV"
