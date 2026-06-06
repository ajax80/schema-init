#!/bin/sh
exec runuser -u ajax80 -- env XDG_RUNTIME_DIR=/run/user/1000 \
    pactl load-module module-tunnel-sink server=192.168.8.246 sink_name=greybox latency_msec=500
