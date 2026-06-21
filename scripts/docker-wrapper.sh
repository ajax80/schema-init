#!/bin/sh
# Explicitly set the PATH so dockerd can locate containerd and other helper binaries
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
exec /usr/bin/dockerd "$@"
