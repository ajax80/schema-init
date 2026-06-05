#!/bin/sh
mkdir -p /run/user/1000
chown 1000:1000 /run/user/1000
chmod 700 /run/user/1000
exec runuser -u ajax80 -- env XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/pipewire
