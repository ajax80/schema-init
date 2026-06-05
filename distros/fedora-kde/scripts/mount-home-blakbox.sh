#!/bin/sh
mount -t btrfs -o subvol=home,compress=zstd:1 UUID=90557be5-57a8-4ff5-bc32-e1bc83be6d75 /home
