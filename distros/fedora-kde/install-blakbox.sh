#!/bin/sh
set -e

if [ "$(id -u)" -ne 0 ]; then
    printf "run as root\n"
    exit 1
fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SVC_DIR="/etc/schema-init/services"
BIN_DIR="/usr/local/bin"

# Desktop user the session/audio services run as. Override: TARGET_USER=foo ./install-blakbox.sh
TARGET_USER="${TARGET_USER:-${SUDO_USER:-$(id -un 1000 2>/dev/null)}}"
TARGET_UID="$(id -u "$TARGET_USER")"
USER_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"

printf "==> writing user.conf (session/audio services read this)\n"
mkdir -p /etc/schema-init
printf 'SCHEMA_USER=%s\nSCHEMA_UID=%s\n' "$TARGET_USER" "$TARGET_UID" > /etc/schema-init/user.conf

printf "==> ensuring 'schema' group (read-only schema-ctl without sudo)\n"
# schema-init opens /run/schema-init.sock to root:schema 0660; members may run
# status/list/timing (reads), writes still require root.
getent group schema >/dev/null || groupadd --system schema
id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx schema || usermod -aG schema "$TARGET_USER"

printf "==> installing services (hosts/blakbox; the old set is moved to /root)\n"
HOST="$REPO/distros/fedora-kde/hosts/blakbox"
LIST="$(mktemp)"
"$REPO/scripts/schema-drift" "$HOST" --list-scripts > "$LIST"
[ -d "$SVC_DIR" ] && mv "$SVC_DIR" "/root/schema-services.bak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SVC_DIR"
cp -a "$HOST/services/." "$SVC_DIR/"
sed -i -e "s|^user=1000\$|user=$TARGET_UID|" -e "s|/run/user/1000/|/run/user/$TARGET_UID/|" \
    "$SVC_DIR/pipewire.svc" "$SVC_DIR/pipewire-pulse.svc" "$SVC_DIR/wireplumber.svc"

printf "==> installing scripts (RPM-owned ones are left to the RPM)\n"
put() { rpm -qf "$2" >/dev/null 2>&1 || install -Dm0755 "$1" "$2"; }
# schema-plasma-autologin.sh calls these to claim and release its session id;
# they go in first so a half-finished install never leaves it without them.
put "$REPO/scripts/schema-session-register"   "$BIN_DIR/schema-session-register"
put "$REPO/scripts/schema-session-unregister" "$BIN_DIR/schema-session-unregister"
while read -r src dst; do
    install -Dm0755 "$src" "$dst"
done < "$LIST"
rm -f "$LIST"
put "$REPO/distros/fedora-kde/scripts/plasma-session-start.sh" "$BIN_DIR/plasma-session-start.sh"
put "$REPO/distros/fedora-kde/scripts/plasmashell-shim"        "$BIN_DIR/plasmashell-shim"

printf "==> building KDE Plasma sd_booted shim (fixes ~30%% idle CPU with no systemd user session)\n"
gcc -shared -fPIC -o /usr/local/lib/mock_sd.so "$REPO/distros/fedora-kde/scripts/mock_sd.c" -ldl
install -d -o "$TARGET_USER" -g "$TARGET_USER" "$USER_HOME/.config/autostart"
cp "$REPO/distros/fedora-kde/config/autostart/org.kde.plasmashell.desktop" "$USER_HOME/.config/autostart/"
chown "$TARGET_USER:$TARGET_USER" "$USER_HOME/.config/autostart/org.kde.plasmashell.desktop"

printf "==> installing user-session autostart runner + plasmashell watchdog\n"
# No `systemd --user` to run xdg-desktop-autostart.target or respawn plasmashell.
# The runner sweeps ~/.config/autostart and keeps plasmashell alive; the env hook
# launches it at session start.
install -d -o "$TARGET_USER" -g "$TARGET_USER" "$USER_HOME/.local/bin"
cp "$REPO/distros/fedora-kde/scripts/schema-autostart-runner.sh" "$USER_HOME/.local/bin/schema-autostart-runner.sh"
chmod +x "$USER_HOME/.local/bin/schema-autostart-runner.sh"
chown "$TARGET_USER:$TARGET_USER" "$USER_HOME/.local/bin/schema-autostart-runner.sh"
cp "$REPO/distros/fedora-kde/scripts/schema-plasma-watchdog.sh" "$USER_HOME/.local/bin/schema-plasma-watchdog.sh"
chmod +x "$USER_HOME/.local/bin/schema-plasma-watchdog.sh"
chown "$TARGET_USER:$TARGET_USER" "$USER_HOME/.local/bin/schema-plasma-watchdog.sh"
install -d -o "$TARGET_USER" -g "$TARGET_USER" "$USER_HOME/.config/plasma-workspace/env"
cp "$REPO/distros/fedora-kde/config/plasma-workspace/env/zz-schema-autostart.sh" "$USER_HOME/.config/plasma-workspace/env/zz-schema-autostart.sh"
chmod +x "$USER_HOME/.config/plasma-workspace/env/zz-schema-autostart.sh"
chown "$TARGET_USER:$TARGET_USER" "$USER_HOME/.config/plasma-workspace/env/zz-schema-autostart.sh"
printf "==> installing plasma session env hooks (flatpak XDG_DATA_DIRS + environment.d replay)\n"
install -d -o "$TARGET_USER" -g "$TARGET_USER" "$USER_HOME/.config/plasma-workspace/env"
cp "$REPO/distros/fedora-kde/config/plasma-env/flatpak-data-dirs.sh" "$USER_HOME/.config/plasma-workspace/env/flatpak-data-dirs.sh"
cp "$REPO/distros/fedora-kde/config/plasma-env/zzz-environment-d.sh" "$USER_HOME/.config/plasma-workspace/env/zzz-environment-d.sh"
chown "$TARGET_USER:$TARGET_USER" \
    "$USER_HOME/.config/plasma-workspace/env/flatpak-data-dirs.sh" \
    "$USER_HOME/.config/plasma-workspace/env/zzz-environment-d.sh"

printf "==> installing dbus policy\n"
mkdir -p /usr/share/dbus-1/system.d
cp "$REPO/distros/shared/dbus/schema-logind.conf" /usr/share/dbus-1/system.d/

printf "\n==> done\n"
printf "New services load at the next boot. schema-drift should now report no drift.\n"
