#!/bin/sh
mkdir -p /home/ajax80/Ocean
mergerfs /mnt/MySpaceDuex:/mnt/Space:/mnt/XtraSpace:/mnt/VostroSpace:/mnt/SeaGate /home/ajax80/Ocean -o defaults,allow_other,use_ino,cache.files=partial,category.create=mspmfs,minfreespace=50G,fsname=Ocean,uid=1000,gid=1000,umask=0022
