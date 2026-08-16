#!/bin/sh
# The ONLY blessed way to back up / roll back the schema-udev E3 flip.
# 08-14: `cp -a` blindly captured whatever was at BIN; a prior aborted flip had
# already left the BROKEN build there, so the rollback binary was the broken one
# (5f2e559f) and the true original (c42164b7) was nearly lost. This refuses to
# back up anything but the known-good baseline, records its md5 in a manifest,
# and on rollback asserts backup==manifest and (binary) backup!=installed.
set -eu

BIN="${SCHEMA_UDEV_BIN:-/usr/bin/schema-udev}"
BAK="${SCHEMA_UDEV_BAK:-/usr/bin/schema-udev.bak-preflip}"
SVC="${SCHEMA_UDEV_SVC:-/etc/schema-init/services}"
SVCBAK="${SCHEMA_UDEV_SVCBAK:-/etc/schema-init/services.bak-preflip}"
MANIFEST="${SCHEMA_UDEV_MANIFEST:-/etc/schema-init/schema-udev.preflip-manifest}"
FLAG="${SCHEMA_UDEV_FLAG:-/etc/schema-init/schema-udev.live}"
GOOD_BASELINE="${SCHEMA_UDEV_GOOD_MD5:-c42164b7f499c47e50182fd0c69be587}"

md5() { md5sum "$1" | cut -d' ' -f1; }
need_root() { [ "${SCHEMA_UDEV_SKIP_ROOT:-0}" = 1 ] || [ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 2; }; }
mval() { sed -n "s/^$1=//p" "$MANIFEST"; }

case "${1:-status}" in
backup)
    need_root
    [ -f "$BIN" ] || { echo "no $BIN" >&2; exit 1; }
    cur="$(md5 "$BIN")"
    if [ "$cur" != "$GOOD_BASELINE" ]; then
        echo "REFUSING BACKUP: $BIN md5=$cur != known-good baseline $GOOD_BASELINE" >&2
        echo "  This is the 08-14 trap: you'd capture a flip build as your rollback source." >&2
        echo "  If this genuinely IS a new intended baseline, re-run with SCHEMA_UDEV_GOOD_MD5=$cur" >&2
        exit 1
    fi
    install -m0755 "$BIN" "$BAK"
    [ "$(md5 "$BAK")" = "$cur" ] || { echo "BACKUP VERIFY FAILED: $BAK md5 mismatch after copy" >&2; exit 1; }
    rm -rf "$SVCBAK"
    cp -a "$SVC" "$SVCBAK"
    printf 'bin_md5=%s\nbaseline=%s\ndate=%s\n' "$cur" "$GOOD_BASELINE" "$(date -Is)" > "$MANIFEST"
    echo "BACKUP OK — bin md5=$cur -> $BAK ; $SVC -> $SVCBAK ; manifest -> $MANIFEST"
    ;;
verify)
    [ -f "$MANIFEST" ] || { echo "no manifest $MANIFEST — run backup first" >&2; exit 1; }
    want="$(mval bin_md5)"
    [ -f "$BAK" ] || { echo "backup binary $BAK missing" >&2; exit 1; }
    got="$(md5 "$BAK")"
    [ "$got" = "$want" ] || { echo "VERIFY FAIL: $BAK md5=$got != manifest $want" >&2; exit 1; }
    [ "$want" = "$GOOD_BASELINE" ] || echo "VERIFY WARN: manifest baseline $want != expected $GOOD_BASELINE" >&2
    [ -d "$SVCBAK" ] || echo "VERIFY WARN: services backup $SVCBAK missing" >&2
    echo "VERIFY OK — backup binary md5=$got matches manifest ($SVCBAK present)"
    ;;
rollback)
    need_root
    [ -f "$MANIFEST" ] || { echo "no manifest $MANIFEST — cannot safely roll back" >&2; exit 1; }
    want="$(mval bin_md5)"
    [ -f "$BAK" ] || { echo "backup binary $BAK missing — cannot roll back" >&2; exit 1; }
    bakmd5="$(md5 "$BAK")"
    [ "$bakmd5" = "$want" ] || { echo "ROLLBACK ABORT: $BAK md5=$bakmd5 != manifest $want (corrupt backup)" >&2; exit 1; }
    # disarm LIVE first so a crash mid-rollback still boots dry-run
    rm -f "$FLAG"
    curmd5="$([ -f "$BIN" ] && md5 "$BIN" || echo none)"
    if [ "$bakmd5" = "$curmd5" ]; then
        echo "note: installed $BIN already == backup ($bakmd5); skipping binary restore, still restoring services"
    else
        install -m0755 "$BAK" "$BIN"   # install, NOT cp: live daemon holds the inode -> cp fails ETXTBSY
        [ "$(md5 "$BIN")" = "$bakmd5" ] || { echo "ROLLBACK VERIFY FAILED: $BIN != backup after restore" >&2; exit 1; }
    fi
    if [ -d "$SVCBAK" ]; then
        rm -rf "$SVC"
        cp -a "$SVCBAK" "$SVC"   # cp, not mv: keep the backup intact for a second attempt
    else
        echo "ROLLBACK WARN: no $SVCBAK — services NOT restored" >&2
    fi
    echo "ROLLBACK OK — $BIN md5=$(md5 "$BIN"); services restored; LIVE flag cleared. Reboot to complete."
    ;;
status)
    [ -f "$MANIFEST" ] && { echo "manifest: $(cat "$MANIFEST" | tr '\n' ' ')"; } || echo "no manifest"
    [ -f "$BAK" ] && echo "backup bin md5: $(md5 "$BAK")" || echo "no backup bin"
    [ -f "$BIN" ] && echo "live   bin md5: $(md5 "$BIN")" || echo "no live bin"
    ;;
*)
    echo "usage: $0 {backup|verify|rollback|status}" >&2; exit 2 ;;
esac
