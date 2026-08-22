#!/bin/bash
# schema first-boot wizard (yad/GTK). Walks the user through the OPTIONAL
# schema-udev flip end to end — and never dead-ends: whatever it finds, it
# explains, saves a report, and leaves the machine in a clean state.
#
# Runs as the UNPRIVILEGED desktop user (XDG autostart on first login + a
# ~/Desktop launcher) so yad draws in the user's Wayland session. Everything
# that needs root is delegated to /usr/local/lib/schema/schema-flip-apply via
# passwordless sudo (see schema.ks sudoers.d + that helper) — the helper is the
# whole privileged surface.
#
# ELIGIBILITY IS PERMISSIVE: the flip proceeds whenever nothing HARMFUL diverges
# (a missing boot/fstab exact-path link, or a missing tag). Harmless supersets
# (extra symlinks, reachable-by-sibling misses) do not block. The headless boot
# seatbelt (schema-udev-flip-healthcheck.sh) auto-rolls-back a flip that comes up
# unusable, so permissive is safe.
#
# The flip is REBOOT-GATED (the daemon reads the LIVE flag at boot):
#   welcome -> [what we found] -> arm -> reboot -> confirm -> done
set -u

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/schema"
STATE="$STATE_DIR/firstboot.state"       # the wizard's own GUI-phase state (user tree)
DESK_ICON="$HOME/Desktop/schema-udev-flip.desktop"
REPORT_USER="$HOME/schema-flip-report.txt"
HELPER=/usr/local/lib/schema/schema-flip-apply

mkdir -p "$STATE_DIR"
[ -f "$STATE" ] || echo welcome > "$STATE"
phase=$(cat "$STATE")

TITLE="schema — finishing setup"
ICON=drive-harddisk

info() { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
             --image="$1" --text="$2" --button="$3":0 "${@:4}"; }

# privileged actions cross to root through the single sudoers-permitted helper.
H() { sudo "$HELPER" "$@"; }

# remove the on-demand launcher (user-owned) and the system autostart (root).
remove_icon()     { rm -f "$DESK_ICON" 2>/dev/null || true; }
stop_autostart()  { H resolve 2>/dev/null || true; }   # drops /etc/xdg/autostart entry
finish_clean()    { stop_autostart; remove_icon; }

# copy the root-written report to the user's home so they can open it without sudo.
pull_report() { local p; p=$(H report 2>/dev/null); [ -n "$p" ] && cp -f "$p" "$REPORT_USER" 2>/dev/null; }

show_report() {
    [ -f "$REPORT_USER" ] || return 0
    yad --title="$TITLE — details" --window-icon="$ICON" --width=820 --height=520 \
        --text-info --filename="$REPORT_USER" --wrap --fontname=monospace \
        --button="Close":0 2>/dev/null || true
}

# translate the seatbelt's raw rollback reason into something a novice reads.
humanize_reason() {
    case "$1" in
        "schema-udev not running")            echo "schema's own device manager didn't start" ;;
        "no /dev/disk/by-uuid entries")       echo "the disks weren't presented the way startup needs" ;;
        "no /dev/input/event"*)               echo "the keyboard and mouse weren't set up" ;;
        "no group-accessible /dev/dri card node") echo "the screen/graphics couldn't be opened" ;;
        "missing core node"*)                 echo "an essential system device was missing" ;;
        "desktop never confirmed"*)           echo "the desktop didn't finish coming up in time" ;;
        "") echo "the switch didn't come up cleanly" ;;
        *)  echo "$1" ;;
    esac
}

# ---------------------------------------------------------------------------
case "$phase" in

welcome)
    info dialog-information \
"<b>Your computer is now running schema.</b>\n\nschema-init has replaced the old startup system. Everything you already \
set up — your login, your desktop — works exactly the same.\n\nThere is one <i>optional</i> extra step. You can skip it and \
your machine is completely finished." "Continue" \
        || { echo skipped > "$STATE"; finish_clean; exit 0; }

    yad --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=applications-system \
        --text="<b>Optional: use schema's own device manager</b>\n\nThis replaces the last piece of the old system. \
It is safe — if anything looks wrong, your computer <b>automatically undoes it on the next restart</b> and goes back to \
exactly how it is now.\n\nWe'll check this machine first and show you what we find." \
        --button="Skip — I'm done":1 --button="Check my machine":0
    [ $? -eq 0 ] || { echo skipped > "$STATE"; finish_clean; exit 0; }

    # always capture the full report, then run the PERMISSIVE eligibility check.
    pull_report
    vout=$(H check 2>&1); vrc=$?
    scanned=$(printf '%s\n' "$vout" | sed -n 's/^devices: \([0-9]*\) scanned.*/\1/p' | tail -1)
    harmful=$(printf '%s\n' "$vout" | sed -n 's/^HARMFUL: \([0-9]*\).*/\1/p' | tail -1)
    inscope=$(printf '%s\n' "$vout" | sed -n 's/^IN-SCOPE DIVERGENCE: \([0-9]*\).*/\1/p' | tail -1)
    harmful=${harmful:-1}; inscope=${inscope:-0}; scanned=${scanned:-0}
    harmless=$(( inscope > harmful ? inscope - harmful : 0 ))

    if [ "$vrc" -ne 0 ] || [ "$harmful" -gt 0 ]; then
        # NOT eligible — but never a silent dead-end. Explain, keep the report,
        # keep the on-demand icon so it can be retried later (e.g. after an
        # update); only stop the every-login autostart nag.
        yad --title="$TITLE" --window-icon="$ICON" --width=580 --borders=18 --image=dialog-warning \
            --text="<b>Not ready to switch on this machine yet.</b>\n\nWe checked <b>${scanned}</b> devices. \
<b>${harmful}</b> need attention before it's safe to switch, so <b>nothing was changed</b> — your computer stays exactly as it is.\n\n\
The full details are saved to:\n<tt>${REPORT_USER}</tt>\n\nYou can show this to someone who can help, then try again later." \
            --button="See details":2 --button="OK, leave it as is":0
        [ $? -eq 2 ] && show_report
        echo welcome > "$STATE"   # retryable: the desktop icon re-runs this check
        stop_autostart            # but stop nagging on every login
        exit 0
    fi

    # ELIGIBLE — summarize, then offer to proceed.
    yad --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=object-select \
        --text="<b>Good — this machine is ready.</b>\n\nWe checked <b>${scanned}</b> devices. \
Any small differences we found (<b>${harmless}</b>) are harmless.\n\nThe switch takes one restart. When your computer comes back \
it confirms everything looks good, and if it doesn't it <b>puts itself back automatically</b> — you don't have to do anything.\n\n\
(A full report was saved to <tt>${REPORT_USER}</tt>.)" \
        --button="Not now":1 --button="Switch and restart":0
    if [ $? -ne 0 ]; then echo welcome > "$STATE"; stop_autostart; exit 0; fi

    if ! H arm; then
        info dialog-error \
"Couldn't prepare the switch, so nothing was changed. Your computer is fine and finished as it is." "OK"
        H disarm || true
        echo welcome > "$STATE"; stop_autostart; exit 0
    fi

    echo armed > "$STATE"
    info dialog-information \
"<b>Ready. Restarting to finish.</b>\n\nWhen your computer comes back it'll confirm everything looks good. \
If it doesn't, it puts itself back the way it is now — you don't have to do anything." "Restart now"
    H reboot
    ;;

armed)
    # booting AFTER the flip was armed. Three outcomes:
    #   1. schema-udev is authoritative        -> success, confirm.
    #   2. the headless seatbelt already healed -> explain WHY, no extra reboot.
    #   3. armed but neither                    -> undo cleanly ourselves.
    rstate=$(H root-state 2>/dev/null)
    if H is-authoritative; then
        H confirm || true            # clears the root state so the seatbelt stops
        pull_report
        echo done > "$STATE"; finish_clean
        info dialog-information \
"<b>All set.</b>\n\nYour computer is now running entirely on schema. There's nothing else to do." "Finish"
    elif [ "$rstate" = skipped ] || [ "$rstate" = done ]; then
        # The seatbelt already rolled us back to the old system on an earlier
        # boot (root state is resolved). Do NOT roll back again or reboot — just
        # tell the user, in plain language, what went wrong, and get out of the way.
        pull_report
        reason=$(humanize_reason "$(H explain 2>/dev/null)")
        echo skipped > "$STATE"; finish_clean
        yad --title="$TITLE" --window-icon="$ICON" --width=600 --borders=18 --image=dialog-warning \
            --text="<b>The switch was undone automatically.</b>\n\nYour computer tried the optional switch, saw that \
<b>${reason}</b>, and put itself back the way it was — on its own, before you even logged in. <b>Everything works normally \
and there's nothing you need to do.</b>\n\nIf you'd like to try again later (or show this to someone who can help), the full \
details are saved to:\n<tt>${REPORT_USER}</tt>" \
            --button="See details":2 --button="OK":0
        [ $? -eq 2 ] && show_report
    else
        # armed, schema-udev isn't authoritative, and the seatbelt hasn't acted
        # (rstate still 'armed'/unknown) -> undo cleanly ourselves.
        pull_report
        reason=$(humanize_reason "$(H explain 2>/dev/null)")
        H rollback || true           # also resets the root state to skipped
        echo skipped > "$STATE"; finish_clean
        info dialog-warning \
"<b>Putting it back the way it was.</b>\n\nThe optional switch didn't take on this hardware (${reason}), so we're undoing it. \
Everything will work normally — one more restart finishes tidying up.\n\n(Details saved to <tt>${REPORT_USER}</tt>.)" "Restart"
        H reboot
    fi
    ;;

*)  # done | skipped: resolved already, get out of the way.
    finish_clean
    ;;
esac
