#!/bin/bash
# schema first-boot wizard (yad/GTK). Walks the user through the OPTIONAL
# schema-udev flip end to end — and never dead-ends: whatever it finds, it
# explains, saves a report, and leaves the machine in a clean state.
#
# Runs as the UNPRIVILEGED desktop user (XDG autostart on first login + a
# ~/Desktop launcher) so yad draws in the user's Wayland session. Everything
# that needs root is delegated to schema-flip-apply (/usr/libexec/schema-init,
# or /usr/local/lib/schema on a no-package install) via passwordless sudo (see
# schema.ks sudoers.d + that helper) — the helper is the whole privileged
# surface.
#
# ELIGIBILITY IS PERMISSIVE: the flip proceeds whenever nothing HARMFUL diverges
# (a missing boot/fstab exact-path link, or a missing tag). Harmless supersets
# (extra symlinks, reachable-by-sibling misses) do not block. The headless boot
# seatbelt (schema-udev-flip-healthcheck.sh) auto-rolls-back a flip that comes up
# unusable, so permissive is safe.
#
# The flip is REBOOT-GATED (the daemon reads the LIVE flag at boot):
#   welcome -> [what we found] -> arm -> reboot -> confirm -> dbus_offer
# then the OPTIONAL schema-dbus flip, one change per reboot, same shape:
#   dbus_offer -> check -> dbus-arm -> reboot -> dbus_armed -> confirm -> done
set -u

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/schema"
STATE="$STATE_DIR/firstboot.state"       # the wizard's own GUI-phase state (user tree)
DESK_ICON="$HOME/Desktop/schema-udev-flip.desktop"
USER_AUTOSTART="$HOME/.config/autostart/schema-firstboot.desktop"
REPORT_USER="$HOME/schema-flip-report.txt"
HELPER=/usr/libexec/schema-init/schema-flip-apply
for h in /usr/libexec/schema-init/schema-flip-apply /usr/local/lib/schema/schema-flip-apply; do
    [ -x "$h" ] && sudo -n -l "$h" >/dev/null 2>&1 && { HELPER=$h; break; }
done

mkdir -p "$STATE_DIR"
[ -f "$STATE" ] || echo welcome > "$STATE"
phase=$(cat "$STATE")

TITLE="schema — finishing setup"
ICON=drive-harddisk

# Only a real choice moves the wizard on. A choice dialog that yad couldn't
# open (rc 1) or that died under it (128+signal: shutdown or logout with it
# still up) is not "Skip" -- that used to record skipped and delete the
# autostart, so a novice who shut down with the wizard open never saw it again.
# Leave the state alone and come back at the next login. 252 is the user
# closing the window: a choice. Notices stay plain yad/info so the cleanup,
# rollback or reboot that follows them still runs.
ask() {
    yad "$@"
    local rc=$?
    if [ "$rc" -eq 1 ] || { [ "$rc" -ge 128 ] && [ "$rc" -ne 252 ]; }; then exit 0; fi
    return "$rc"
}

info() { yad --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 \
             --image="$1" --text="$2" --button="$3":0 "${@:4}"; }

# privileged actions cross to root through the single sudoers-permitted helper.
H() { sudo "$HELPER" "$@"; }

# remove the on-demand launcher (user-owned) and the system autostart (root).
remove_icon()     { rm -f "$DESK_ICON" 2>/dev/null || true; }
stop_autostart()  { H resolve 2>/dev/null || true; rm -f "$USER_AUTOSTART"; }
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
        "system bus is not schema-dbus"*)     echo "schema's own message bus didn't start" ;;
        "system bus not serving"*)            echo "system services couldn't reach each other over the message bus" ;;
        "") echo "the switch didn't come up cleanly" ;;
        *)  echo "$1" ;;
    esac
}

# ---------------------------------------------------------------------------
case "$phase" in

welcome)
    ask --title="$TITLE" --window-icon="$ICON" --width=520 --borders=18 --image=dialog-information --button="Continue":0 --text=\
"<b>Your computer is now running schema.</b>\n\nschema-init has replaced the old startup system. Everything you already \
set up — your login, your desktop — works exactly the same.\n\nThere is one <i>optional</i> extra step. You can skip it and \
your machine is completely finished." \
        || { echo skipped > "$STATE"; finish_clean; exit 0; }

    ask --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=applications-system \
        --text="<b>Optional: use schema's own device manager</b>\n\nThis replaces the last piece of the old system. \
It is safe — if anything looks wrong, your computer <b>automatically undoes it on the next restart</b> and goes back to \
exactly how it is now.\n\nWe'll check this machine first and show you what we find." \
        --button="Skip — I'm done":3 --button="Check my machine":0
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
    ask --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=object-select \
        --text="<b>Good — this machine is ready.</b>\n\nWe checked <b>${scanned}</b> devices. \
Any small differences we found (<b>${harmless}</b>) are harmless.\n\nThe switch takes one restart. When your computer comes back \
it confirms everything looks good, and if it doesn't it <b>puts itself back automatically</b> — you don't have to do anything.\n\n\
(A full report was saved to <tt>${REPORT_USER}</tt>.)" \
        --button="Not now":3 --button="Switch and restart":0
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
        echo dbus_offer > "$STATE"
        exec "$0"
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

dbus_offer)
    ask --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=applications-system \
        --text="<b>The device manager switch worked.</b>\n\nThere is one last <i>optional</i> step: use schema's own \
<b>message bus</b> — the channel your desktop and system services talk over. Like before, if anything looks wrong your \
computer <b>automatically undoes it on the next restart</b>.\n\nYou can skip it and your machine is completely finished." \
        --button="Skip — I'm done":3 --button="Check my machine":0
    [ $? -eq 0 ] || { echo done > "$STATE"; finish_clean; exit 0; }

    if ! why=$(H dbus-check 2>&1); then
        yad --title="$TITLE" --window-icon="$ICON" --width=580 --borders=18 --image=dialog-warning \
            --text="<b>Not ready to switch the message bus on this machine yet.</b>\n\n<b>Nothing was changed</b> — \
your computer stays exactly as it is.\n\n<tt>${why}</tt>" \
            --button="OK, leave it as is":0
        stop_autostart
        exit 0
    fi

    ask --title="$TITLE" --window-icon="$ICON" --width=560 --borders=18 --image=object-select \
        --text="<b>Good — this machine is ready.</b>\n\nThe switch takes one restart. When your computer comes back \
it confirms everything looks good, and if it doesn't it <b>puts itself back automatically</b>." \
        --button="Not now":3 --button="Switch and restart":0
    if [ $? -ne 0 ]; then stop_autostart; exit 0; fi

    if ! H dbus-arm; then
        info dialog-error \
"Couldn't prepare the switch, so nothing was changed. Your computer is fine and finished as it is." "OK"
        H dbus-rollback || true
        stop_autostart; exit 0
    fi

    echo dbus_armed > "$STATE"
    info dialog-information \
"<b>Ready. Restarting to finish.</b>\n\nWhen your computer comes back it'll confirm everything looks good. \
If it doesn't, it puts itself back the way it is now — you don't have to do anything." "Restart now"
    H reboot
    ;;

dbus_armed)
    rstate=$(H dbus-state 2>/dev/null)
    if H dbus-is-authoritative; then
        H dbus-confirm || true
        pull_report
        echo done > "$STATE"; finish_clean
        info dialog-information \
"<b>All set.</b>\n\nYour computer is now running entirely on schema. There's nothing else to do." "Finish"
    elif [ "$rstate" = skipped ]; then
        reason=$(humanize_reason "$(H dbus-explain 2>/dev/null)")
        echo done > "$STATE"; finish_clean
        info dialog-warning \
"<b>The message bus switch was undone automatically.</b>\n\nYour computer saw that <b>${reason}</b> and put itself \
back the way it was. <b>Everything works normally and there's nothing you need to do.</b>" "OK"
    else
        reason=$(humanize_reason "$(H dbus-explain 2>/dev/null)")
        H dbus-rollback || true
        echo done > "$STATE"; finish_clean
        info dialog-warning \
"<b>Putting it back the way it was.</b>\n\nThe optional message bus switch didn't take on this hardware \
(${reason}), so we're undoing it. One more restart finishes tidying up." "Restart"
        H reboot
    fi
    ;;

*)  # done | skipped: resolved already, get out of the way.
    finish_clean
    ;;
esac
