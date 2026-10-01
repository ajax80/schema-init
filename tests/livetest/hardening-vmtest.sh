#!/bin/bash
# Hardening default-flip VM test (spec 2026-09-26-service-hardening-default-flip).
# Boots schema-init as PID 1 several times, varying only the host switch
# (/etc/schema-init/hardening-default) and schema.hardening_default= on the
# cmdline. Every boot carries the same probe services; the off boots are the
# anti-false-green control for the on boots.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE}"
REPO="${REPO:-$HOME/projects/schema-init}"
KERNEL="${KERNEL:-/lib/modules/$(uname -r)/vmlinuz}"
[ -r "$KERNEL" ] || KERNEL="/boot/vmlinuz-$(uname -r)"
BB="/usr/sbin/busybox"
WORK="$(mktemp -d /var/tmp/schema-hardening-vmtest.XXXXXX)"
TIMEOUT="${TIMEOUT:-60}"
REBUILD="${REBUILD:-1}"
trap 'rm -rf "$WORK"' EXIT
echo ">> workdir: $WORK"

if [ "$REBUILD" = 1 ]; then
  echo ">> building schema-init ($(git -C "$REPO" rev-parse --abbrev-ref HEAD))"
  make -C "$REPO" >/dev/null 2>&1 || { echo "BUILD FAILED"; make -C "$REPO"; exit 1; }
fi
BIN="$REPO/schema-init"
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }
MOUNTNS="$WORK/test_mountns"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$MOUNTNS" "$HERE/test_mountns.c" \
  || { echo "MOUNTNS HELPER BUILD FAILED"; exit 1; }
LANDLOCK="$WORK/test_landlock"
cc -static -O2 -std=c11 -D_GNU_SOURCE -o "$LANDLOCK" "$HERE/test_landlock.c" \
  || { echo "LANDLOCK HELPER BUILD FAILED"; exit 1; }
SCTL="$WORK/schema-ctl"
cc -static -O2 -std=c99 -D_GNU_SOURCE -I"$REPO" -o "$SCTL" "$REPO/schema-ctl.c" \
  || { echo "SCHEMA-CTL STATIC BUILD FAILED"; exit 1; }

TAGS="unset z-nnp z-pt z-ps z-ph zero homeexec"
LLTAGS="ll-on ll-nnp ll-off ll-nnp-off"

build_root() {
  local root="$1" switch="$2"
  mkdir -p "$root"/{sbin,bin,usr/bin,etc/schema-init/services,proc,sys,dev,run,tmp,var/tmp}
  mkdir -p "$root"/{boot/efi,home/jon,root,etc/ll,srv/ll,usr/libexec}
  cp "$BIN" "$root/sbin/schema-init"
  ln -sf /sbin/schema-init "$root/init"
  cp "$BB" "$root/bin/busybox"
  for a in sh ls cat sleep mount echo grep head kill readlink true; do
    ln -sf /bin/busybox "$root/bin/$a"
  done
  cp "$MOUNTNS" "$root/bin/test_mountns"
  cp "$SCTL" "$root/bin/schema-ctl"
  cp "$MOUNTNS" "$root/home/jon/test_mountns"
  : > "$root/root/secret"
  cp "$LANDLOCK" "$root/bin/test_landlock"
  cp "$BB" "$root/usr/libexec/true"
  : > "$root/etc/ll/conf"
  : > "$root/etc/ll-other"
  chmod 0777 "$root/etc/ll" "$root/srv/ll"
  [ "$switch" = ABSENT ] || printf '%b' "$switch" > "$root/etc/schema-init/hardening-default"

  local S="$root/etc/schema-init/services"
  svc() { printf 'name=%s\nexec=/bin/test_mountns\nargs=%s\nneeds_root=1\n%b' "$1" "$1" "${2:-}" > "$S/$1.svc"; }
  svc unset
  svc z-nnp 'no_new_privs=0\n'
  svc z-pt  'private_tmp=0\n'
  svc z-ps  'protect_system=0\n'
  svc z-ph  'protect_home=0\n'
  svc zero  'no_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\n'
  printf 'name=homeexec\nexec=/home/jon/test_mountns\nargs=homeexec\nneeds_root=1\n' > "$S/homeexec.svc"
  ll() { printf 'name=%s\nexec=/bin/test_landlock\nargs=%s\nneeds_root=1\n%b' "$1" "$1" "${2:-}" > "$S/$1.svc"; }
  local LLP='landlock_ro=/bin\nlandlock_ro=/etc/ll\nlandlock_rw=/srv/ll\nlandlock_ro=/nonexistent\n'
  ll ll-on  "no_new_privs=0\n$LLP"
  ll ll-nnp "no_new_privs=1\nkeep_caps=CAP_NET_BIND_SERVICE\n$LLP"
  ll ll-off
  ll ll-nnp-off 'no_new_privs=1\nkeep_caps=CAP_NET_BIND_SERVICE\n'
  ll ll-bad 'landlock_ro=/etc/ll\n'
  ll ll-nocap "no_new_privs=0\nkeep_caps=CAP_NET_BIND_SERVICE\n$LLP"
  printf 'name=explicit-bad\nexec=/home/jon/test_mountns\nargs=explicit-bad\nneeds_root=1\nprotect_home=1\n' > "$S/explicit-bad.svc"

  cat > "$root/usr/bin/hvfinish.sh" <<EOF
#!/bin/sh
exec >/dev/console 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do
  n=0; for t in $TAGS; do [ -e /run/mountns-\$t ] && n=\$((n+1)); done
  for t in $LLTAGS; do grep -qs "^LL \$t:" /var/log/schema-init/\$t.log /run/log/schema-init/\$t.log && n=\$((n+1)); done
  [ \$n = $(echo $TAGS $LLTAGS | wc -w) ] && break
  sleep 1
done
NS1=\$(readlink /proc/1/ns/mnt)
echo "===== HV-REPORT ====="
for t in $TAGS; do
  P=\$(head -1 /sys/fs/cgroup/schema-init/\$t/cgroup.procs 2>/dev/null)
  set -- \$(grep NoNewPrivs /proc/\$P/status 2>/dev/null)
  NNP=\${2:-?}
  [ "\$(readlink /proc/\$P/ns/mnt 2>/dev/null)" = "\$NS1" ] && NS=same || NS=own
  [ -e /tmp/mountns-sentinel-\$t ] && TMP=host || TMP=private
  echo "HV \$t: nnp=\$NNP ns=\$NS tmp=\$TMP \$(cat /run/mountns-\$t 2>/dev/null || echo NOREPORT)"
done
[ -e /run/mountns-explicit-bad ] && echo "HV explicit-bad: RAN" || echo "HV explicit-bad: NOTRUN"
for t in $LLTAGS ll-bad ll-nocap; do
  cat /var/log/schema-init/\$t.log /run/log/schema-init/\$t.log 2>/dev/null | while read -r l; do echo "HVLL \$t| \$l"; done
done
/bin/schema-ctl status ll-on | while read -r l; do echo "HVCTL ll-on| \$l"; done
for t in unset z-pt homeexec nosuch; do
  /bin/schema-ctl status \$t | while read -r l; do echo "HVCTL \$t| \$l"; done
done
echo "===== HV-END ====="
( sleep 20; echo "SHUTDOWN-WEDGED"; poweroff -f ) &
kill -INT 1
EOF
  chmod +x "$root/usr/bin/hvfinish.sh"
  printf 'name=hv-finish\nexec=/usr/bin/hvfinish.sh\noneshot=1\nneeds_root=1\nno_new_privs=0\nprivate_tmp=0\nprotect_system=0\nprotect_home=0\n' > "$S/hv-finish.svc"
  for t in $TAGS $LLTAGS; do echo "dep=$t" >> "$S/hv-finish.svc"; done
}

boot() {
  local name="$1" switch="$2" extra="$3" dir="$WORK/$1"
  build_root "$dir/root" "$switch"
  ( cd "$dir/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$dir/initramfs.cpio.gz"
  timeout "$TIMEOUT" qemu-system-x86_64 \
    -enable-kvm -m 512 -smp 2 -no-reboot -nographic -nic none \
    -kernel "$KERNEL" -initrd "$dir/initramfs.cpio.gz" \
    -append "console=ttyS0 rdinit=/sbin/schema-init panic=1 loglevel=4 $extra" \
    < /dev/null > "$dir/serial.log" 2>&1 || true
  cat "$dir/serial.log" >> "$OUT/last-hardening-vmtest-serial.log"
}

OFF_ALL="nnp=0 ns=same tmp=host usr=OPEN etc=OPEN efi=OPEN home=OPEN root=OPEN"
ON_UNSET="nnp=1 ns=own tmp=private usr=RO etc=OPEN efi=RO home=EMPTY root=EMPTY"
pass=1
want() {  # boot tag expected-substring
  local line
  line=$(grep -a "^HV $2:" "$WORK/$1/serial.log" | tr -d '\r' | head -1)
  case "$line" in
    *"$3"*) ;;
    *) echo "  MISS [$1] $2: want '$3' got '${line:-<none>}'"; pass=0 ;;
  esac
}
ctl() {  # boot svc regex
  grep -a "^HVCTL $2|" "$WORK/$1/serial.log" | tr -d '\r' | grep -Eq "$3" \
    || { echo "  MISS [$1] schema-ctl status $2: no line matching '$3'"; pass=0; }
}
logged() {  # boot regex label
  grep -aEq "$2" "$WORK/$1/serial.log" || { echo "  MISS [$1] $3"; pass=0; }
}
llwant() {  # boot svc expected-substring
  grep -a "^HVLL $2|" "$WORK/$1/serial.log" | tr -d '\r' | grep -qF "$3" \
    || { echo "  MISS [$1] $2: want '$3' got '$(grep -a "^HVLL $2|" "$WORK/$1/serial.log" | tr -d '\r' | head -3)'"; pass=0; }
}
common() {  # boot on|off
  logged "$1" "hardening default: $( [ "$2" = on ] && echo ON || echo off )" "hardening default log ($2)"
  logged "$1" "explicit-bad: protect_home hides exec — not loaded" "explicit conflicting knob not refused"
  want "$1" explicit-bad "NOTRUN"
  logged "$1" "HV-END" "finisher never reported"
  logged "$1" "PID 1 reboot" "shutdown rail did not reach reboot()"
  if grep -aq SHUTDOWN-WEDGED "$WORK/$1/serial.log"; then echo "  MISS [$1] shutdown wedged"; pass=0; fi
}

: > "$OUT/last-hardening-vmtest-serial.log"

echo ">> boot absent (control)";           boot absent  ABSENT ""
echo ">> boot file-on";                    boot fileon  'on\n' ""
echo ">> boot file-on + cmdline=0";        boot rescue  'on\n' "schema.hardening_default=0"
echo ">> boot absent + cmdline=1";         boot trial   ABSENT "schema.hardening_default=1"
echo ">> boot file ' on \\\\n\\\\n'";      boot padded  '  on \n\n' ""
echo ">> boot file 'ON'";                  boot upper   'ON\n' ""
echo ">> boot file '1'";                   boot one     '1\n' ""

for b in absent rescue upper one; do
  common "$b" off
  for t in $TAGS; do want "$b" "$t" "$OFF_ALL"; done
done
for b in fileon trial padded; do
  common "$b" on
  want "$b" unset "$ON_UNSET"
done
want fileon z-nnp "nnp=0 ns=own tmp=private usr=RO etc=OPEN efi=RO home=EMPTY root=EMPTY"
want fileon z-pt  "nnp=1 ns=own tmp=host usr=RO etc=OPEN efi=RO home=EMPTY root=EMPTY"
want fileon z-ps  "nnp=1 ns=own tmp=private usr=OPEN etc=OPEN efi=OPEN home=EMPTY root=EMPTY"
want fileon z-ph  "nnp=1 ns=own tmp=private usr=RO etc=OPEN efi=RO home=OPEN root=OPEN"
want fileon zero  "$OFF_ALL"
want fileon homeexec "nnp=1 ns=own tmp=private usr=RO etc=OPEN efi=RO home=OPEN root=OPEN"
logged fileon "homeexec: default protect_home would hide exec — dropped" "defaulted conflict not dropped+logged"

LL_ON="uid=0 rd_in=OK rd_out=DENIED wr_rw=OK wr_ro=DENIED dir_out=DENIED exec_in=OK exec_out=DENIED"
LL_OFF="uid=0 rd_in=OK rd_out=OK wr_rw=OK wr_ro=OK dir_out=OK exec_in=OK exec_out=OK"
for b in absent fileon; do
  llwant "$b" ll-on  "LL ll-on: $LL_ON"
  llwant "$b" ll-nnp "LL ll-nnp: $LL_ON"
  llwant "$b" ll-off "LL ll-off: $LL_OFF"
  llwant "$b" ll-nnp-off "LL ll-nnp-off: $LL_OFF"
  logged "$b" "ll-bad: landlock does not cover exec — not loaded" "ll-bad not refused"
  logged "$b" "ll-nocap: landlock needs no_new_privs=1" "ll-nocap not refused"
  ctl "$b" ll-on "landlock_rw +/srv/ll"
  ctl "$b" ll-on "landlock_ro +/nonexistent"
done

ctl absent unset  "hardening default: off"
ctl absent unset  "no_new_privs +0 +unset"
ctl absent z-pt   "private_tmp +0 +explicit"
ctl fileon unset  "hardening default: ON"
ctl fileon unset  "no_new_privs +1 +default"
ctl fileon unset  "protect_system +1 +default"
ctl fileon z-pt   "private_tmp +0 +explicit"
ctl fileon z-pt   "protect_home +1 +default"
ctl fileon homeexec "protect_home +0 +dropped"
ctl fileon homeexec "private_tmp +1 +default"
ctl fileon nosuch "err: not found: nosuch"

echo ">> serial: $OUT/last-hardening-vmtest-serial.log"
if [ "$pass" = 1 ]; then
  echo ">> RESULT: PASS  (7 boots: switch off/on/rescue/trial/format, per-knob =0, default-conflict drop, landlock)"
else
  echo ">> RESULT: FAIL"
  trap - EXIT; echo "   workdir kept: $WORK"
  exit 1
fi
