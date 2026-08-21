#!/bin/bash
# schema first-boot wizard (yad/GTK — looks native on GNOME's Adwaita).
#
# Runs as the UNPRIVILEGED desktop user — from the XDG autostart on first login
# and from the ~/Desktop launcher — so yad draws in the user's Wayland session.
# Everything that needs root is delegated to /usr/local/lib/schema/schema-flip-apply
# via passwordless sudo (see schema.ks sudoers.d rule + that helper). This split
# is deliberate: root + a GUI on the user's Wayland session can't be the same
# process here (no polkit auth agent — the KDE one is a systemd *user* unit that
# cannot run under schema-init), so the GUI stays user-side and only the closed
# set of privileged actions crosses to root.
#
# Two phases, because the schema-udev flip is REBOOT-GATED (the daemon reads the
# LIVE flag at boot, not live):
#   welcome -> [offer flip] -> arm -> reboot -> verify -> done
# A separate headless oneshot (schema-udev-flip-healthcheck.sh) auto-rolls-back a
# flip that breaks boot, so a bad flip never strands a user at a black screen.
#
# INTEGRATION POINTS (marked ***): the "is the flip healthy?" check and the
# acceptable-divergence threshold are the two things to tune against real hardware.
set -u

# state lives in the USER's tree — the wizard runs unprivileged, so /var/lib is
# not writable here. (The headless root healthcheck keeps its own state.)
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/schema"
STATE="$STATE_DIR/firstboot.state"
SYS_AUTOSTART=/etc/xdg/autostart/schema-firstboot.desktop     # root-owned; removed via helper
DESK_ICON="$HOME/Desktop/schema-udev-flip.desktop"            # user-owned; removed here
HELPER=/usr/local/lib/schema/schema-flip-apply

mkdir -p "$STATE_DIR"
[ -f "$STATE" ] || echo welcome > "$STATE"
phase=$(cat "$STATE")

TITLE="schema — finishing setup"
ICON=drive-harddisk

info()  { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
              --image="$1" --text="$2" --button="$3":0 "${@:4}"; }

# privileged actions cross to root through the single sudoers-permitted helper.
H() { sudo "$HELPER" "$@"; }

# once resolved, never nag again: drop the system autostart (root) + our own icon.
disable_launchers() { H resolve 2>/dev/null || true; rm -f "$DESK_ICON" 2>/dev/null || true; }

# *** INTEGRATION POINT: is schema-udev actually the authority right now? ***
udev_is_authoritative() { H is-authoritative; }

# ---------------------------------------------------------------------------
case "$phase" in

welcome)
    info dialog-information \
"<b>Your computer is now running schema.</b>\n\nschema-init has replaced the old startup system. Everything you already \
set up — your login, your desktop — works exactly the same.\n\nThere is one <i>optional</i> extra step. You can skip it and \
your machine is completely finished." "Continue" || { echo skipped > "$STATE"; disable_launchers; exit 0; }

    yad --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=applications-system \
        --text="<b>Optional: use schema's own device manager</b>\n\nThis replaces the last piece of the old system. \
It is safe — if anything looks wrong, your computer <b>automatically undoes it on the next restart</b> and goes back to \
exactly how it is now.\n\nOnly do this if you'd like to; otherwise choose <b>Skip</b>." \
        --button="Skip — I'm done":1 --button="Set it up":0
    [ $? -eq 0 ] || { echo skipped > "$STATE"; disable_launchers; exit 0; }

    # parity gate BEFORE we touch anything (root helper; reads shadow rule-data).
    # `check` exits 0 only when in-scope divergence is 0; scrape the number only
    # to show the user.
    vout=$(H check 2>&1); vrc=$?
    div=$(printf '%s\n' "$vout" | sed -n 's/.*IN-SCOPE DIVERGENCE: \([0-9]*\).*/\1/p' | tail -1)
    if [ "$vrc" -ne 0 ]; then
        info dialog-warning \
"<b>Not a match on this hardware yet.</b>\n\nschema's device manager found <b>${div:-some}</b> differences on this machine, \
so we won't switch — your computer stays exactly as it is now. Nothing was changed." "OK, leave it as is"
        echo skipped > "$STATE"; disable_launchers; exit 0
    fi

    # back up + arm, all root-side in one helper call.
    if ! H arm; then
        info dialog-error \
"Couldn't prepare the switch, so nothing was changed. Your computer is fine and finished as it is." "OK"
        H disarm || true
        echo skipped > "$STATE"; disable_launchers; exit 0
    fi

    echo armed > "$STATE"
    info dialog-information \
"<b>Ready. One restart to finish.</b>\n\nWhen your computer comes back, it'll confirm everything looks good. \
If it doesn't, it puts itself back the way it is now — you don't have to do anything." "Restart now"
    H reboot
    ;;

armed)
    # we are booting AFTER the flip was armed. Did it come up healthy?
    if udev_is_authoritative; then
        echo done > "$STATE"; disable_launchers
        info dialog-information \
"<b>All set.</b>\n\nYour computer is now running entirely on schema. There's nothing else to do." "Finish"
    else
        # desktop came up but udev isn't authoritative -> undo cleanly
        H rollback || true
        echo skipped > "$STATE"; disable_launchers
        info dialog-warning \
"<b>Put back the way it was.</b>\n\nThe extra step didn't take on this hardware, so your computer undid it automatically. \
Everything works normally — one more restart will tidy up." "Restart"
        H reboot
    fi
    ;;

*)  # done | skipped | anything else: resolved already, get out of the way
    disable_launchers
    ;;
esac
