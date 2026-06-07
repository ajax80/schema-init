#!/bin/sh
exec runuser -u ajax80 -- env XDG_RUNTIME_DIR=/run/user/1000 GIO_USE_VFS=local /usr/bin/wireplumber
