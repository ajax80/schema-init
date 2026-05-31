#!/bin/sh
pipewire &
sleep 1
wireplumber &
sleep 1
pipewire-pulse &
