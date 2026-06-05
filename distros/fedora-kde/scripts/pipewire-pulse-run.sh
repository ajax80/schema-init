#!/bin/sh
exec runuser -u ajax80 -- env XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/pipewire-pulse
