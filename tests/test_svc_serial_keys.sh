#!/bin/sh
# Every .svc key the parser accepts must be covered by a row of svc_serial.c's
# props[] table, so a new key can't silently escape the verify baseline.
set -eu
cd "$(dirname "$0")/.."

# name: it is the prefix of every serialized line, not a property of its own
allow="name"

keys=$( {
    grep -o 'strcmp(\(line\|key\), "[^"]*"' service.c | sed 's/.*"\(.*\)"/\1/'
    grep -o '{ "limit_[a-z]*"' service.c | sed 's/.*"\(.*\)"/\1/'
    for d in $(sed -n 's/.*service_exec_dir_key\[EXEC_DIR_KINDS\] = {\(.*\)};/\1/p' service.c | tr -d '",'); do
        echo "$d"; echo "${d}_mode"
    done
    grep -o 'line + 7, "[a-z_]*"' service.c | sed 's/.*"\(.*\)"/verify_\1/'
    sed -n '/ns_kernel_names\[NS_K_COUNT\] = {/,/};/p' ns.c | grep -o '"[a-z_]*"' | tr -d '"'
} | sort -u)

[ "$(echo "$keys" | wc -l)" -gt 100 ] || { echo "FAIL svc_serial_keys: only $(echo "$keys" | wc -l) parser keys found"; exit 1; }

table=$(sed -n '/props\[\] = {/,/^};/p' svc_serial.c 2>/dev/null || true)
miss=""
for k in $keys; do
    case " $allow " in *" $k "*) continue ;; esac
    printf '%s\n' "$table" | grep -q "\"$k\"" || miss="$miss $k"
done
if [ -n "$miss" ]; then
    echo "FAIL svc_serial_keys: parser keys with no serializer row:$miss"
    exit 1
fi
echo "ok svc_serial_keys ($(echo "$keys" | wc -l) keys)"
