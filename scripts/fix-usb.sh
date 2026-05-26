#!/bin/bash
set -e

DEV=${1:-/dev/sde}
MNT=/mnt/schema-usb

echo "=== Mounting ==="
mkdir -p "$MNT"
mount "${DEV}2" "$MNT"
mount "${DEV}1" "$MNT/boot/efi"
mount --bind /dev     "$MNT/dev"
mount --bind /dev/pts "$MNT/dev/pts"
mount --bind /proc    "$MNT/proc"
mount --bind /sys     "$MNT/sys"
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

echo "=== Building schema-init static ==="
cd /home/ajax80/projects/schema-init
make clean
gcc -std=c99 -Wall -O2 -D_GNU_SOURCE -static \
    -o "$MNT/sbin/schema-init" \
    init.c schema.c service.c -lrt
chmod +x "$MNT/sbin/schema-init"
echo "Binary: $(file $MNT/sbin/schema-init)"

echo "=== Installing packages ==="
chroot "$MNT" /bin/bash <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q isc-dhcp-client iproute2 2>/dev/null || true
apt-get install -y task-cinnamon-desktop lightdm xserver-xorg-input-all xserver-xorg-input-libinput \
    gnome-terminal network-manager elogind libpam-elogind policykit-1 2>/dev/null || true
pam-auth-update --enable elogind 2>/dev/null || true
CHROOT

echo "=== Writing service files ==="
mkdir -p "$MNT/etc/schema-init/services"

cat > "$MNT/etc/schema-init/services/getty-tty1.svc" <<'SVC'
name=getty-tty1
exec=/sbin/agetty
args=--autologin
args=root
args=tty1
args=linux
oneshot=0
needs_root=1
critical=1
SVC

cat > "$MNT/usr/local/sbin/schema-network" <<'NET'
#!/bin/sh
for iface in $(ls /sys/class/net); do
    [ "$iface" = "lo" ] && continue
    ip link set "$iface" up
    dhclient -1 "$iface" && break
done
NET
chmod +x "$MNT/usr/local/sbin/schema-network"

cat > "$MNT/etc/schema-init/services/network.svc" <<'SVC'
name=network
exec=/usr/local/sbin/schema-network
oneshot=1
needs_root=1
critical=0
SVC

cat > "$MNT/usr/local/sbin/schema-dbus" <<'DBUS'
#!/bin/sh
mkdir -p /run/dbus
dbus-uuidgen --ensure
exec /usr/bin/dbus-daemon --system --nofork
DBUS
chmod +x "$MNT/usr/local/sbin/schema-dbus"

cat > "$MNT/etc/schema-init/services/dbus.svc" <<'SVC'
name=dbus
exec=/usr/local/sbin/schema-dbus
oneshot=0
needs_root=1
critical=0
SVC

cat > "$MNT/usr/local/sbin/schema-udev" <<'UDEV'
#!/bin/sh
/lib/systemd/systemd-udevd &
sleep 2
udevadm trigger --action=add || true
udevadm settle --timeout=30 || true
UDEV
chmod +x "$MNT/usr/local/sbin/schema-udev"

cat > "$MNT/etc/schema-init/services/udev.svc" <<'SVC'
name=udev
exec=/usr/local/sbin/schema-udev
oneshot=1
needs_root=1
critical=0
SVC

cat > "$MNT/etc/schema-init/services/network-manager.svc" <<'SVC'
name=network-manager
exec=/usr/sbin/NetworkManager
dep=dbus
oneshot=0
needs_root=1
critical=0
SVC

cat > "$MNT/etc/schema-init/services/elogind.svc" <<'SVC'
name=elogind
exec=/usr/lib/elogind/elogind
dep=dbus
oneshot=0
needs_root=1
critical=0
SVC

printf 'name=polkit\nexec=/usr/lib/polkit-1/polkitd\ndep=dbus\noneshot=0\nneeds_root=1\ncritical=0\n' \
    > "$MNT/etc/schema-init/services/polkit.svc"

cat > "$MNT/etc/schema-init/services/display-manager.svc" <<'SVC'
name=display-manager
exec=/usr/sbin/lightdm
dep=dbus
dep=udev
dep=elogind
dep=polkit
oneshot=0
needs_root=1
critical=0
SVC

echo "=== Configuring lightdm autologin ==="
mkdir -p "$MNT/etc/lightdm/lightdm.conf.d"
cat > "$MNT/etc/lightdm/lightdm.conf.d/50-autologin.conf" <<'CONF'
[Seat:*]
autologin-user=root
autologin-user-timeout=0
user-session=cinnamon
CONF

echo "=== Allowing Cinnamon to run as root ==="
grep -q CINNAMON_BYPASS_ROOT_CHECK "$MNT/etc/environment" 2>/dev/null || \
    printf '\nCINNAMON_BYPASS_ROOT_CHECK=1\n' >> "$MNT/etc/environment"

echo "=== Fixing session D-Bus (no logind) ==="
cat > "$MNT/etc/X11/Xsession.d/19-no-logind" <<'XSESS'
#!/bin/sh
RDIR="/run/user/$(id -u)"
mkdir -p "$RDIR"
chmod 700 "$RDIR"
export XDG_RUNTIME_DIR="$RDIR"
if [ ! -S "$RDIR/bus" ]; then
    /usr/bin/dbus-daemon --session \
        --address="unix:path=$RDIR/bus" \
        --nofork --nopidfile &
    sleep 0.3
fi
export DBUS_SESSION_BUS_ADDRESS="unix:path=$RDIR/bus"
XSESS
chmod +x "$MNT/etc/X11/Xsession.d/19-no-logind"

echo "=== Unblocking root autologin in PAM ==="
sed -i 's/^auth.*pam_succeed_if.so user != root.*/#&/' "$MNT/etc/pam.d/lightdm-autologin"

echo "=== Setting xterm as default terminal and starting PulseAudio ==="
mkdir -p "$MNT/etc/dconf/db/local.d" "$MNT/etc/dconf/profile"
printf '[org/cinnamon/desktop/applications/terminal]\nexec='"'"'xterm'"'"'\nexec-arg='"'"'-e'"'"'\n' \
    > "$MNT/etc/dconf/db/local.d/00-schema"
printf 'user-db:user\nsystem-db:local\n' > "$MNT/etc/dconf/profile/user"
chroot "$MNT" dconf update 2>/dev/null || true

cat > "$MNT/etc/X11/Xsession.d/70-pulseaudio" <<'XSESS'
#!/bin/sh
if which pulseaudio >/dev/null 2>&1; then
    pulseaudio --start --exit-idle-time=-1 2>/dev/null || true
fi
XSESS
chmod +x "$MNT/etc/X11/Xsession.d/70-pulseaudio"

echo "=== Terminal wrapper ==="
printf '#!/bin/sh\nexec xterm "$@"\n' > "$MNT/usr/local/bin/gnome-terminal"
chmod +x "$MNT/usr/local/bin/gnome-terminal"

echo "=== Shutdown binaries ==="
for dir in "$MNT/sbin" "$MNT/usr/sbin"; do
    printf '#!/bin/sh\nkill -TERM 1\n' > "$dir/poweroff"
    printf '#!/bin/sh\nkill -TERM 1\n' > "$dir/halt"
    printf '#!/bin/sh\nkill -INT 1\n'  > "$dir/reboot"
    printf '#!/bin/sh\ncase "$1" in\n  -r|--reboot) kill -INT 1 ;;\n  *) kill -TERM 1 ;;\nesac\n' > "$dir/shutdown"
    chmod +x "$dir/poweroff" "$dir/halt" "$dir/reboot" "$dir/shutdown"
done

echo "=== Fixing GRUB config ==="
chroot "$MNT" /bin/bash <<'CHROOT'
cat > /etc/default/grub <<'GRUB'
GRUB_DEFAULT=0
GRUB_TIMEOUT=3
GRUB_DISTRIBUTOR="schema-init"
GRUB_CMDLINE_LINUX_DEFAULT="rw init=/sbin/schema-init"
GRUB_CMDLINE_LINUX=""
GRUB
update-grub
CHROOT

echo "=== Unmounting ==="
umount "$MNT/dev/pts"
umount "$MNT/dev"
umount "$MNT/proc"
umount "$MNT/sys"
umount "$MNT/boot/efi"
umount "$MNT"

echo "=== USB ready ==="
