#!/bin/sh
# schema-init sysprep — one-shot, runs every boot before the desktop rail.
# Lays the runtime scaffolding a systemd-less box otherwise never gets:
# runtime dirs, tmpfiles, per-user XDG_RUNTIME_DIR, cold-plugged /dev, and the
# render/input group grants the autologin compositor needs to open the GPU.
mkdir -p /run/systemd/system /run/dbus /run/udev /run/lock /run/user 2>/dev/null
rm -f /run/nologin 2>/dev/null
/usr/bin/systemd-tmpfiles --create 2>/dev/null

# cold-plug /dev so the DRM card, input devices and /dev/disk/by-uuid exist
# before mount-fstab and the compositor come up.
/usr/lib/systemd/systemd-udevd --daemon
udevadm trigger --type=subsystems --action=add
udevadm trigger --type=devices --action=add
udevadm settle --timeout=30

# XDG_RUNTIME_DIR per user — logind normally delegates this to a systemd unit
# that cannot run here, so create it for root and every human account.
for uid in 0 $(awk -F: '$3>=1000 && $3<65000 {print $3}' /etc/passwd); do
    d=/run/user/$uid
    mkdir -p "$d" && chown "$uid:$uid" "$d" 2>/dev/null && chmod 700 "$d"
done

# kwin's DRM backend opens the card directly (NoopSession) when it cannot get a
# device fd from logind; that needs the login user in video/input/render. Grant
# it here so a fresh install's user (whatever Dad named it) can start a session.
for u in $(awk -F: '$3>=1000 && $3<65000 {print $1}' /etc/passwd); do
    usermod -aG video,input,render "$u" 2>/dev/null || true
done

exit 0
