#!/bin/sh
# Load system and user profiles to establish proper environment variables (PATH, XDG_DATA_DIRS, etc.)
if [ -f /etc/profile ]; then
    . /etc/profile
fi
if [ -f "$HOME/.profile" ]; then
    . "$HOME/.profile"
elif [ -f "$HOME/.bash_profile" ]; then
    . "$HOME/.bash_profile"
fi

/usr/bin/startplasma-wayland &
SPW=$!
i=0
while [ $i -lt 60 ]; do
    [ -S ${XDG_RUNTIME_DIR}/wayland-0 ] && break
    sleep 0.5
    i=$((i+1))
done
export WAYLAND_DISPLAY=wayland-0

# Start the PipeWire stack. Under systemd these are user units; with schema-init
# as PID 1 there is no systemd --user to launch them, so nothing provides the
# pipewire-0 socket the block below waits for -> no audio, and kpipewire spins
# retrying the connection. Start them here, in-session (seat access -> real ALSA
# sinks). Guarded: skip if something already runs them (e.g. a host that does).
if [ -x /usr/bin/pipewire ] && ! pgrep -x pipewire >/dev/null 2>&1; then
    /usr/bin/pipewire &
    sleep 1
    /usr/bin/wireplumber &
    sleep 1
    /usr/bin/pipewire-pulse &
fi

i=0
while [ $i -lt 20 ]; do
    [ -S ${XDG_RUNTIME_DIR}/pipewire-0 ] && break
    sleep 0.5
    i=$((i+1))
done
sleep 1
pgrep -x plasmashell > /dev/null || /usr/local/bin/plasmashell-shim &

# Plasma daemons that are normally systemd --user units. Their /etc/xdg/autostart
# entries carry X-systemd-skip=true, so with no systemd --user the KDE autostart
# fallback skips them too -> no power management (PowerDevil settings "service not
# running"), no global shortcuts, no polkit auth prompts. Start them in-session so
# they inherit the logind session (polkit-kde needs it to resolve its subject).
for _svc in /usr/libexec/org_kde_powerdevil \
            /usr/libexec/kglobalacceld \
            /usr/libexec/kf6/polkit-kde-authentication-agent-1; do
    [ -x "$_svc" ] && ! pgrep -f "$_svc" > /dev/null 2>&1 && "$_svc" &
done

wait $SPW
