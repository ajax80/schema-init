#!/bin/sh
fail=0
for t in tests/test_*.py; do
    case $t in
        tests/test_dbus_gate.py|tests/test_migrate_seatbelt.py|tests/test_logind_vt.py)
            echo "=== $t: skipped (needs a delegated cgroup, live udev or real device nodes)"; continue ;;
    esac
    echo "=== $t"
    QT_QPA_PLATFORM=offscreen python3 "$t" || fail=1
done
[ "$fail" = 0 ] || { echo "PYTHON SUITE FAILED"; exit 1; }
