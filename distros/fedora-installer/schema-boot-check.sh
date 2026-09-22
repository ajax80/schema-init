#!/bin/bash
# schema boot check — click after `dnf update`, before rebooting, to confirm
# the schema boot entries are intact. Runs the full schema-doctor suite
# (already fast, already running unattended every 10 min) and reports on
# just the boot-entry-integrity result. Privileged work crosses to root
# through the one sudoers-permitted command below — that's the whole
# privileged surface (see schema-boot-check.sudoers), matching
# firstboot-flip-wizard.sh's H() pattern.
set -u

TITLE="schema — boot check"
ICON=drive-harddisk

info() { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
             --image="$1" --text="$2" --button="$3":0 "${@:4}" 2>/dev/null; }

report_json="$(sudo /usr/local/bin/schema-doctor --heal --json 2>/dev/null)"
if [ -z "$report_json" ]; then
    info dialog-error "<b>Couldn't run the boot check.</b>\n\nschema-doctor didn't return a report. \
Don't reboot until you've looked into this manually." "Close"
    exit 1
fi

mapfile -t fields < <(printf '%s' "$report_json" | python3 -c '
import json, sys
try:
    checks = json.load(sys.stdin)
except Exception:
    print("error")
    print("Could not parse schema-doctor output.")
    print("")
    sys.exit(0)
for c in checks:
    if c.get("name") == "boot-entry-integrity":
        print(c.get("state", "unknown"))
        print((c.get("detail", "") or "").replace(chr(10), " "))
        print((c.get("action", "") or "").replace(chr(10), " "))
        sys.exit(0)
print("missing")
print("boot-entry-integrity check not found in report")
print("")
')

state="${fields[0]:-error}"
detail="${fields[1]:-}"
action="${fields[2]:-}"

case "$state" in
    clean)
        info dialog-information "<b>Boot entries OK.</b>\n\nSafe to reboot." "Close"
        ;;
    healed)
        info dialog-warning "<b>Fixed the boot entries.</b>\n\n${action}\n\nSafe to reboot now." \
             "Close" --button="Details":1
        ;;
    reported)
        info dialog-error "<b>Could not fix — do not reboot.</b>\n\n${detail}" "Close"
        ;;
    *)
        info dialog-error "<b>Unexpected result.</b>\n\n${detail}\n\nDo not reboot until this is understood." "Close"
        ;;
esac
