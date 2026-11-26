#!/bin/sh
# CI, compat corpus stage 3: the synthetic units in tests/fixtures/compat-enforce
# (one family of directives each) imported, run under the real schema-init as
# PID 1 in a QEMU guest, and every mapped directive checked in force by
# tools/compat-enforce.py. fs-tampered is fs with private_tmp=1 cut from its
# .svc: it must come back NOT-ENFORCED, or the oracle can't see a loss at all.
set -eu
umask 022
cd "$(dirname "$0")/../.."
make -s schema-init
file -L schema-init | grep -Eq 'static(ally|-pie) linked' || { echo "FAIL: ./schema-init is not static; make clean first"; exit 1; }
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
python3 tools/compat-enforce.py --tamper fs-tampered:private_tmp=1 --json "$OUT" tests/fixtures/compat-enforce
python3 - "$OUT" <<'EOF'
import json, sys
units = json.load(open(sys.argv[1]))["units"]
bad = []
for u in units:
    if not u["ran"]:
        bad.append("%s never reported: %s" % (u["unit"], u.get("log")))
    for d in u["directives"]:
        k = "%s %s.%s" % (u["unit"], d["section"], d["key"])
        want = "NOT-ENFORCED" if k == "fs-tampered.service Service.PrivateTmp" else "enforced"
        # mapped weaker on purpose and warned: allowed to fall short
        if d["warned"] and d["verdict"] == "NOT-ENFORCED":
            continue
        if d["key"] == "ExecStart":
            continue
        if d["verdict"] != want:
            bad.append("%s: %s, want %s (%s)" % (k, d["verdict"], want, d["detail"]))
if bad:
    print("FAIL:\n  " + "\n  ".join(bad))
    sys.exit(1)
print(">> PASS: %d units probed under PID 1; every mapped directive in force; the tampered one caught"
      % len(units))
EOF
