#!/bin/sh
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/^Version:[[:space:]]*//p' schema-init.spec | head -1)
case "${1:-}" in
stable) VER=$V-1 ;;
dev) VER=$V-1.$(git rev-list --count HEAD).$(git log -1 --format=%cd --date=format:%Y%m%d)git$(git rev-parse --short=7 HEAD) ;;
*) echo "usage: $0 stable|dev" >&2; exit 2 ;;
esac
cat > debian/changelog <<EOT
schema-init ($VER) $1; urgency=medium

  * Automated $1 build.

 -- Jonathan Ayers <ayersjon80@gmail.com>  $(git log -1 --format=%cD)
EOT
echo "$VER"
