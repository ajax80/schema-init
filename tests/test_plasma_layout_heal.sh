#!/bin/sh
# plasma-session-start.sh's first-run layout heal, run on its own: a panel saved
# half-built (an applet section with no plugin=: no tray, no clock) is set aside
# so plasmashell builds the default again; a healthy layout is never touched.
set -u
cd "$(dirname "$0")/.."
S=distros/fedora-kde/scripts/plasma-session-start.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL plasma_layout_heal: $*"; exit 1; }
sed -n '/^_schema_heal_layout() {$/,/^}$/p' "$S" > "$T/fn.sh"
[ -s "$T/fn.sh" ] || fail "no _schema_heal_layout() in $S"
heal() { sh -c ". '$T/fn.sh'; _schema_heal_layout '$1'"; }

L=$T/appletsrc
# the v0.4.31 isotest file: [7] (the tray) lost its plugin=, [8]/[9] never written
cat > "$L" <<'L'
[Containments][2]
formfactor=2
plugin=org.kde.panel
wallpaperplugin=org.kde.image

[Containments][2][Applets][3]
immutability=1
plugin=org.kde.plasma.kickoff

[Containments][2][Applets][3][Configuration]
popupHeight=400

[Containments][2][Applets][6]
immutability=1
plugin=org.kde.plasma.marginsseparator

[Containments][2][Applets][7]
wallpaperplugin=org.kde.image

[Containments][2][Applets][7][Applets][10]
immutability=1
plugin=org.kde.plasma.cameraindicator
L
heal "$L"
[ ! -e "$L" ] || fail "a broken layout was left in place"
ls "$T"/appletsrc.broken-* >/dev/null 2>&1 || fail "the broken layout was not kept aside"
grep -q "Applets\]\[7\]" "$T"/appletsrc.broken-* || fail "the kept copy is not the original"

# healthy: every applet section has plugin= (subsections like [Configuration] and
# a tray's own [Applets] need none of their own)
rm -f "$T"/appletsrc.broken-*
cat > "$L" <<'L'
[Containments][2]
plugin=org.kde.panel

[Containments][2][Applets][3]
plugin=org.kde.plasma.kickoff

[Containments][2][Applets][3][Configuration][General]
icon=start-here

[Containments][2][Applets][7]
plugin=org.kde.plasma.systemtray

[Containments][2][Applets][7][Configuration]
SystrayContainmentId=8

[Containments][8][Applets][10]
plugin=org.kde.plasma.cameraindicator
L
cp "$L" "$T/orig"
heal "$L"
cmp -s "$L" "$T/orig" || fail "a healthy layout was changed"
ls "$T"/appletsrc.broken-* >/dev/null 2>&1 && fail "a healthy layout was set aside"

# no layout yet (first ever start): nothing to do, no error
rm -f "$L"
heal "$L" || fail "missing layout: nonzero"
[ ! -e "$L" ] || fail "missing layout: something was created"
echo "plasma_layout_heal: ok"
