#!/bin/sh
# schema-init lacks the /dev/fd + std{in,out,err} symlinks that standard init
# creates after mounting devtmpfs. Without them, bash process substitution
# (< <(...)) and /dev/stdin etc. fail. Recreate them each boot (devtmpfs is
# volatile). Superseded once mount_pseudo() in schema-init init.c does this.
ln -sfn /proc/self/fd   /dev/fd
ln -sfn /proc/self/fd/0 /dev/stdin
ln -sfn /proc/self/fd/1 /dev/stdout
ln -sfn /proc/self/fd/2 /dev/stderr
