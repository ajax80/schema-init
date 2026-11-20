#!/bin/sh
# Every .svc key the parser accepts must be covered by a row of svc_serial.c's
# props[] table, so a new key can't silently escape the verify baseline.
set -eu
cd "$(dirname "$0")/.."

# name: it is the prefix of every serialized line, not a property of its own
allow="name"

# each source checked on its own: a pattern that stops matching after a
# refactor would otherwise just shrink the set and pass
strcmp_keys=$(grep -o 'strcmp(\(line\|key\), "[^"]*"' service.c | sed 's/.*"\(.*\)"/\1/' | sort -u)
limit_keys=$(grep -o '{ "limit_[a-z]*"' service.c | sed 's/.*"\(.*\)"/\1/' | sort -u)
dir_keys=$(for d in $(sed -n 's/.*service_exec_dir_key\[EXEC_DIR_KINDS\] = {\(.*\)};/\1/p' service.c | tr -d '",'); do
    echo "$d"; echo "${d}_mode"
done)
verify_keys=$(grep -o 'line + 7, "[a-z_]*"' service.c | sed 's/.*"\(.*\)"/verify_\1/' | sort -u)
ns_keys=$(sed -n '/ns_kernel_names\[NS_K_COUNT\] = {/,/};/p' ns.c | grep -o '"[a-z_]*"' | tr -d '"')

atleast() {
    n=$(printf '%s\n' "$2" | grep -c . || true)
    [ "$n" -ge "$3" ] || { echo "FAIL svc_serial_keys: only $n $1 keys found (want >= $3)"; exit 1; }
}
atleast strcmp "$strcmp_keys" 100
atleast limit "$limit_keys" 16
atleast exec_dir "$dir_keys" 8
atleast verify "$verify_keys" 3
atleast ns_kernel "$ns_keys" 7

keys=$(printf '%s\n' "$strcmp_keys" "$limit_keys" "$dir_keys" "$verify_keys" "$ns_keys" | sort -u)

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
