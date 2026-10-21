#!/bin/sh
# Header-defined static state included by two PID 1 objects gives each its own copy.
dups=$(for o in "$@"; do nm "$o" | awk -v o="$o" '$2 ~ /^[bBdD]$/ && $3 !~ /\./ {print $3, o}'; done |
       sort | awk '{c[$1]++; f[$1]=f[$1]" "$2} END {for (k in c) if (c[k] > 1) print k ":" f[k]}')
if [ -n "$dups" ]; then
    echo "check_tu_statics: state duplicated across objects:"; echo "$dups"; exit 1
fi
echo "check_tu_statics: ok"
