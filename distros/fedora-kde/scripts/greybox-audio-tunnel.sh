#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-ajax80}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
SINK_SERVER="${SINK_SERVER:-}"
[ -z "$SINK_SERVER" ] && { echo "greybox-audio: SINK_SERVER unset in /etc/schema-init/user.conf" >&2; exit 0; }
exec runuser -u "$SCHEMA_USER" -- env XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" \
    pactl load-module module-tunnel-sink server="$SINK_SERVER" sink_name=greybox latency_msec=500
