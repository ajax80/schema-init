#!/bin/sh
[ -r /etc/schema-init/user.conf ] && . /etc/schema-init/user.conf
SCHEMA_USER="${SCHEMA_USER:-$(awk -F: '$3>=1000 && $3<65000 {print $1; exit}' /etc/passwd)}"
SCHEMA_UID="${SCHEMA_UID:-1000}"
[ -z "$SCHEMA_USER" ] && { echo "pipewire-run: no desktop user found — set SCHEMA_USER in /etc/schema-init/user.conf" >&2; exit 0; }
mkdir -p "/run/user/$SCHEMA_UID"
chown "$SCHEMA_UID:$SCHEMA_UID" "/run/user/$SCHEMA_UID"
chmod 700 "/run/user/$SCHEMA_UID"
exec prlimit --rtprio=70 --nice=39 --memlock=4294967296 --nofile=1048576 -- setpriv --reuid="$SCHEMA_USER" --regid="$(id -g "$SCHEMA_USER")" --init-groups -- env HOME="$(getent passwd "$SCHEMA_USER" | cut -d: -f6)" USER="$SCHEMA_USER" LOGNAME="$SCHEMA_USER" XDG_RUNTIME_DIR="/run/user/$SCHEMA_UID" /usr/bin/pipewire
