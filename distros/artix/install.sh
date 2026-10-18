#!/bin/sh
set -e
P=distros/artix
L=/usr/local/libexec/schema-init
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -f $P/install.sh ] || { echo "run from the top of the schema-init tree" >&2; exit 1; }

if [ "${1:-}" = --remove ]; then
    rm -f /etc/grub.d/11_schema-init
    rm -rf $L
    grub-mkconfig -o /boot/grub/grub.cfg
    echo "schema-init boot entries removed. /etc/schema-init and the binaries in /usr/bin are left in place."
    exit 0
fi

make install
install -d -m 0755 /etc/schema-init/services $L/shipped
install -m 0755 scripts/schema-mount-fstab scripts/schema-dbus-run.sh $P/scripts/artix-boot $P/scripts/artix-coldplug $P/scripts/artix-sysctl $P/scripts/artix-halt $L/
install -m 0644 tools/dbus-learn/dissect_policy.py $L/
for f in $P/services/*.svc; do
    n=$(basename "$f"); t=/etc/schema-init/services/$n
    if [ -e "$t" ] && ! cmp -s "$t" "$f" && ! cmp -s "$t" "$L/shipped/$n"; then
        echo "kept your edited $t (new version: $f)"
    else
        install -m 0644 "$f" "$t"
        install -m 0644 "$f" "$L/shipped/$n"
    fi
done

install -m 0755 distros/devuan/grub.d/11_schema-init /etc/grub.d/11_schema-init
grub-mkconfig -o /boot/grub/grub.cfg
if [ "$(cat /proc/1/comm)" = schema-init ]; then
    schema-ctl reexec || echo "PID 1 keeps running the previous schema-init until the next reboot."
fi
echo
echo "Installed. Your current init is still the default boot entry."
echo "Reboot and pick a 'schema-init:' entry in the GRUB menu to try it."
