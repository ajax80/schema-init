#!/bin/sh
uid=$(id -u)
export XDG_RUNTIME_DIR=/run/user/$uid
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
pipewire &
sleep 1
wireplumber &
sleep 1
pipewire-pulse &
