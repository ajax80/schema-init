#!/bin/sh
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
    rm -f /usr/local/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service
    update-grub
    echo "schema-init boot entries removed. /etc/schema-init and the binaries in /usr/bin are left in place."
    exit 0
fi

make install
[ -x /usr/sbin/logrotate ] || { cmp -s /etc/schema-init/services/logrotate.svc services/logrotate.svc && rm -f /etc/schema-init/services/logrotate.svc; }
install -d -m 0755 /etc/schema-init/services $L/sysv
install -d -m 0755 $L/shipped
dm=$(basename "$(cat /etc/X11/default-display-manager 2>/dev/null)")
for f in $P/services/*.svc; do
    n=$(basename "$f"); t=/etc/schema-init/services/$n
    case "$n" in
        slim.svc|lightdm.svc)
            if [ "$n" != "$dm.svc" ]; then
                cmp -s "$t" "$L/shipped/$n" && rm -f "$t" "$L/shipped/$n"
                continue
            fi ;;
    esac
    x=$(sed -n 's/^exec=//p' "$f")
    if [ ! -x "$x" ]; then
        cmp -s "$t" "$L/shipped/$n" && rm -f "$t" "$L/shipped/$n"
        continue
    fi
    if [ -e "$t" ] && ! cmp -s "$t" "$f" && ! cmp -s "$t" "$L/shipped/$n"; then
        echo "kept your edited $t (new version: $f)"
    else
        install -m 0644 "$f" "$t"
        install -m 0644 "$f" "$L/shipped/$n"
    fi
done
install -m 0755 scripts/schema-mount-fstab $P/scripts/devuan-boot $P/scripts/devuan-coldplug $P/scripts/schema-halt $L/

for f in halt shutdown; do
    dpkg-divert --quiet --rename --add --divert /usr/sbin/$f.sysvinit /usr/sbin/$f
    ln -sf $L/schema-halt /usr/sbin/$f
done
for n in halt reboot poweroff; do ln -sf /usr/sbin/halt.sysvinit $L/sysv/$n; done
ln -sf /usr/sbin/shutdown.sysvinit $L/sysv/shutdown

install -m 0755 $P/grub.d/11_schema-init /etc/grub.d/11_schema-init
update-grub
if [ "$(cat /proc/1/comm)" = schema-init ]; then
    schema-ctl reexec || echo "PID 1 keeps running the previous schema-init until the next reboot."
fi
[ -e "/etc/schema-init/services/$dm.svc" ] || echo "No service for display manager '${dm:-none}': write /etc/schema-init/services/$dm.svc (run it in the foreground)."
echo
echo "Installed. sysvinit is still the default boot entry."
echo "Reboot and pick a 'schema-init:' entry in the GRUB menu to try it."
