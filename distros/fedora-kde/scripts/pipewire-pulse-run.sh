#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-$(awk -F: '$3>=1000 && $3<65000 {print $1; exit}' /etc/passwd)}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
exec runuser -u "$SCHEMA_USER" -- env XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" /usr/bin/pipewire-pulse
