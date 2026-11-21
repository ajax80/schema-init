#!/bin/sh
# Committed font files must match a regen from tools/ (make font). Skipped
# without python3 so building/testing schema-init never needs it.
command -v python3 >/dev/null || { echo "font drift: skipped (no python3)"; exit 0; }
T=${TESTDIR:-build/test}
mkdir -p "$T"
python3 -I tools/mkschemafont.py fonts/spleen-12x24.bdf "$T/sf.bdf" &&
python3 -I tools/bdf2psf.py "$T/sf.bdf" "$T/sf.psfu" &&
python3 -I tools/psf2c.py "$T/sf.psfu" "$T/sf.c" &&
cmp -s "$T/sf.bdf" fonts/schema-12x24.bdf && cmp -s "$T/sf.psfu" fonts/schema-12x24.psfu &&
cmp -s "$T/sf.c" schema_font.c || { echo "schema font: committed files drifted from tools/ (make font)"; exit 1; }
echo "font drift: ok"
