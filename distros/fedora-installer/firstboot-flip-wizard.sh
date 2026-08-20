#!/bin/bash
# schema first-boot wizard (yad/GTK — looks native on GNOME's Adwaita).
#
# Runs once, guarded by a state file, as an XDG autostart on the first login
# after Anaconda finishes. It reads as the tail of the install to a novice.
#
# Two phases, because the schema-udev flip is REBOOT-GATED (the daemon reads
# the LIVE flag at boot, not live):
#   welcome -> [offer flip] -> backup+arm -> reboot -> verify -> done
# The GUI is only the happy path. A separate headless oneshot
# (schema-udev-flip-healthcheck.sh) auto-rolls-back a flip that breaks boot,
# so a bad flip never strands a user at a black screen with no way back.
#
# INTEGRATION POINTS (marked ***): the "is the flip healthy?" check and the
# acceptable-divergence threshold are the two things to tune against real
# hardware before this ships.
set -u

STATE_DIR=/var/lib/schema-init
STATE=$STATE_DIR/firstboot.state
AUTOSTART=/etc/xdg/autostart/schema-firstboot.desktop
LIB=/usr/local/lib/schema
ARM="$LIB/schema-udev-flip-arm.sh"
BACKUP="$LIB/schema-udev-flip-backup.sh"
VERIFY="$LIB/verify-rules-live"
SHIP_MD5_FILE=/etc/schema-init/schema-udev.ship-md5

mkdir -p "$STATE_DIR"
[ -f "$STATE" ] || echo welcome > "$STATE"
phase=$(cat "$STATE")

TITLE="schema — finishing setup"
ICON=drive-harddisk

info()  { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
              --image="$1" --text="$2" --button="$3":0 "${@:4}"; }
disable_autostart() { rm -f "$AUTOSTART"; }   # never nag again once resolved

# *** INTEGRATION POINT: is schema-udev actually the authority right now? ***
# Refine against the real /run/schema-udev sentinel. For now: daemon alive,
# LIVE flag armed, and udevadm (compat) still answers = device db is populated.
udev_is_authoritative() {
    pgrep -x schema-udev >/dev/null 2>&1 || return 1
    "$ARM" status | grep -q '^ARMED' || return 1
    udevadm info /dev/null >/dev/null 2>&1 || return 1
    return 0
}

sudo_run() { pkexec "$@"; }   # GUI privilege prompt, Dad-friendly

# ---------------------------------------------------------------------------
case "$phase" in

welcome)
    info dialog-information \
"<b>Your computer is now running schema.</b>\n\nschema-init has replaced the old startup system. Everything you already \
set up — your login, your desktop — works exactly the same.\n\nThere is one <i>optional</i> extra step. You can skip it and \
your machine is completely finished." "Continue" || { echo skipped > "$STATE"; disable_autostart; exit 0; }

    yad --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=applications-system \
        --text="<b>Optional: use schema's own device manager</b>\n\nThis replaces the last piece of the old system. \
It is safe — if anything looks wrong, your computer <b>automatically undoes it on the next restart</b> and goes back to \
exactly how it is now.\n\nOnly do this if you'd like to; otherwise choose <b>Skip</b>." \
        --button="Skip — I'm done":1 --button="Set it up":0
    [ $? -eq 0 ] || { echo skipped > "$STATE"; disable_autostart; exit 0; }

    # parity gate BEFORE we touch anything. verify-rules-live exits 0 only when
    # in-scope divergence is 0 (its own E3-flip precondition). Gate on that exit
    # code; scrape the reported number only to show the user.
    vout=$("$VERIFY" 2>&1); vrc=$?
    div=$(printf '%s\n' "$vout" | sed -n 's/.*IN-SCOPE DIVERGENCE: \([0-9]*\).*/\1/p' | tail -1)
    if [ "$vrc" -ne 0 ]; then
        info dialog-warning \
"<b>Not a match on this hardware yet.</b>\n\nschema's device manager found <b>${div:-some}</b> differences on this machine, \
so we won't switch — your computer stays exactly as it is now. Nothing was changed." "OK, leave it as is"
        echo skipped > "$STATE"; disable_autostart; exit 0
    fi

    # bless THIS iso's shipped binary as the rollback baseline, back up, arm
    ship_md5=$(cat "$SHIP_MD5_FILE" 2>/dev/null || echo "")
    if ! sudo_run env SCHEMA_UDEV_GOOD_MD5="$ship_md5" "$BACKUP" backup \
         || ! sudo_run "$BACKUP" verify \
         || ! sudo_run "$ARM" arm; then
        info dialog-error \
"Couldn't prepare the switch, so nothing was changed. Your computer is fine and finished as it is." "OK"
        sudo_run "$ARM" disarm || true
        echo skipped > "$STATE"; disable_autostart; exit 0
    fi

    echo armed > "$STATE"
    info dialog-information \
"<b>Ready. One restart to finish.</b>\n\nWhen your computer comes back, it'll confirm everything looks good. \
If it doesn't, it puts itself back the way it is now — you don't have to do anything." "Restart now"
    sudo_run systemctl reboot 2>/dev/null || sudo_run schema-ctl reboot
    ;;

armed)
    # we are booting AFTER the flip was armed. Did it come up healthy?
    if udev_is_authoritative; then
        echo done > "$STATE"; disable_autostart
        info dialog-information \
"<b>All set.</b>\n\nYour computer is now running entirely on schema. There's nothing else to do." "Finish"
    else
        # desktop came up but udev isn't authoritative -> undo cleanly
        sudo_run "$BACKUP" rollback || true
        echo skipped > "$STATE"; disable_autostart
        info dialog-warning \
"<b>Put back the way it was.</b>\n\nThe extra step didn't take on this hardware, so your computer undid it automatically. \
Everything works normally — one more restart will tidy up." "Restart"
        sudo_run systemctl reboot 2>/dev/null || sudo_run schema-ctl reboot
    fi
    ;;

*)  # done | skipped | anything else: resolved already, get out of the way
    disable_autostart
    ;;
esac
