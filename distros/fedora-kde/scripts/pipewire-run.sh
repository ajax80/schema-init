#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-ajax80}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
mkdir -p "/run/user/$SCHEMA_UID"
chown "$SCHEMA_UID:$SCHEMA_UID" "/run/user/$SCHEMA_UID"
chmod 700 "/run/user/$SCHEMA_UID"
exec runuser -u "$SCHEMA_USER" -- env XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" /usr/bin/pipewire
