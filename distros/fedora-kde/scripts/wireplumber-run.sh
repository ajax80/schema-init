#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-ajax80}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
exec runuser -u "$SCHEMA_USER" -- env XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" GIO_USE_VFS=local /usr/bin/wireplumber
