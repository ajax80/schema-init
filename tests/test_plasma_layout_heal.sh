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

# an orphan applet group (no plugin=) in a lived-in layout that still has its
# clock: not the first-run cut — never reset someone's desktop for it (review)
cat > "$L" <<'L'
[Containments][2]
plugin=org.kde.panel

[Containments][2][Applets][3]
plugin=org.kde.plasma.kickoff

[Containments][2][Applets][15]
immutability=1

[Containments][2][Applets][20]
plugin=org.kde.plasma.digitalclock
L
cp "$L" "$T/orig"
heal "$L"
cmp -s "$L" "$T/orig" || fail "an orphan group with the clock present reset the layout"

# cut even earlier: a panel with no applets at all, no clock
cat > "$L" <<'L'
[Containments][1]
plugin=org.kde.plasma.folder

[Containments][2]
formfactor=2
plugin=org.kde.panel
L
heal "$L"
[ ! -e "$L" ] || fail "an empty panel was left in place"
rm -f "$T"/appletsrc.broken-*

# the heal only runs where plasmashell is about to be started, never under a live one
sed -n '/^_schema_start_plasmashell() {$/,/^}$/p' "$S" > "$T/start.sh"
[ -s "$T/start.sh" ] || fail "no _schema_start_plasmashell() in $S"
printf '[Containments][2]\nplugin=org.kde.panel\n' > "$L"
mkdir -p "$T/bin"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/pgrep"; chmod +x "$T/bin/pgrep"
printf '#!/bin/sh\necho started > "%s/started"\n' "$T" > "$T/bin/plasmashell"; chmod +x "$T/bin/plasmashell"
PATH="$T/bin:$PATH" HOME="$T/home" sh -c ". '$T/fn.sh'; . '$T/start.sh'; _uid=0 PLASMASHELL='$T/bin/plasmashell' SCHEMA_LAYOUT='$L' _schema_start_plasmashell; wait"
[ -e "$L" ] || fail "the heal moved the layout while plasmashell was running"
[ ! -e "$T/started" ] || fail "a second plasmashell was started"
printf '#!/bin/sh\nexit 1\n' > "$T/bin/pgrep"
PATH="$T/bin:$PATH" HOME="$T/home" sh -c ". '$T/fn.sh'; . '$T/start.sh'; _uid=0 PLASMASHELL='$T/bin/plasmashell' SCHEMA_LAYOUT='$L' _schema_start_plasmashell; wait"
[ ! -e "$L" ] || fail "no plasmashell running: the broken layout was not healed before starting it"
[ -e "$T/started" ] || fail "plasmashell was not started"

# no layout yet (first ever start): nothing to do, no error
rm -f "$L"
heal "$L" || fail "missing layout: nonzero"
[ ! -e "$L" ] || fail "missing layout: something was created"
echo "plasma_layout_heal: ok"
