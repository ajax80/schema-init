#!/bin/sh
# The styled rail on a real (UEFI framebuffer) VT: vmtest with VT=1, screens saved as PNG.
exec env VT=1 bash "$(dirname "$0")/vmtest.sh" "$@"
