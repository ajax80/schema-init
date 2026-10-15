#!/bin/sh
# Run from the top of the schema-init tree after `make`:
#   sudo distros/devuan/install.sh            install
#   sudo distros/devuan/install.sh --remove   undo (sysvinit stays the default either way)
set -e
P=distros/devuan
L=/usr/local/libexec/schema-init
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -f $P/install.sh ] || { echo "run from the top of the schema-init tree" >&2; exit 1; }

if [ "${1:-}" = --remove ]; then
    rm -f /etc/grub.d/11_schema-init
    for f in halt shutdown; do
        rm -f /usr/sbin/$f
        dpkg-divert --quiet --rename --remove /usr/sbin/$f
    done
    rm -rf $L
    update-grub
    echo "schema-init boot entries removed. /etc/schema-init and the binaries in /usr/bin are left in place."
    exit 0
fi

make install
install -d -m 0755 /etc/schema-init/services $L/sysv
install -m 0644 $P/services/*.svc /etc/schema-init/services/
install -m 0755 scripts/schema-mount-fstab $P/scripts/devuan-boot $P/scripts/devuan-coldplug $P/scripts/schema-halt $L/

for f in halt shutdown; do
    dpkg-divert --quiet --rename --add --divert /usr/sbin/$f.sysvinit /usr/sbin/$f
    ln -sf $L/schema-halt /usr/sbin/$f
done
for n in halt reboot poweroff; do ln -sf /usr/sbin/halt.sysvinit $L/sysv/$n; done
ln -sf /usr/sbin/shutdown.sysvinit $L/sysv/shutdown

install -m 0755 $P/grub.d/11_schema-init /etc/grub.d/11_schema-init
update-grub
echo
echo "Installed. sysvinit is still the default boot entry."
echo "Reboot and pick a 'schema-init:' entry in the GRUB menu to try it."
