#!/bin/bash
# boot-capture.sh — full boot-analysis snapshot for schema-init.
# Run after a boot (needs sudo for dmesg + ctl). Writes a timestamped report
# under boot-logs/ and appends one CSV row to boot-logs/boot-history.csv so
# you can track boot performance across reboots over time.
#
# schema-init gives children NO PATH (PID 1 environ is HOME/TERM/split_lock_detect
# only, and service.c spawns via execv) — so an absolute shebang and an explicit
# PATH are mandatory when this runs as boot-timing.svc. Same trap that broke
# docker.svc. HOME=/ for the same reason, so ~ lookups fall back to hostname.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
set -u

LOGDIR="$(dirname "$(readlink -f "$0")")/boot-logs"
mkdir -p "$LOGDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="$LOGDIR/boot-$STAMP.txt"
CSV="$LOGDIR/boot-history.csv"
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi
SCTL="$SUDO schema-ctl"

{
  printf '====================================================\n'
  printf ' schema-init boot analysis — %s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
  printf '====================================================\n\n'

  printf '### HOST\n'
  printf 'node:        %s\n' "$(cat ~/.claude/NODE 2>/dev/null || hostname)"
  printf 'hostname:    %s\n' "$(hostname)"
  printf 'kernel:      %s\n' "$(uname -r)"
  printf 'booted at:   %s\n' "$(uptime -s)"
  printf 'uptime now:  %s\n\n' "$(uptime -p)"

  printf '### KERNEL CMDLINE\n%s\n\n' "$(cat /proc/cmdline)"

  printf '### PID 1 (this is schema-init, not systemd)\n'
  ps -o pid,ppid,rss,vsz,comm -p 1
  printf 'PID 1 RSS:   %s  (systemd-as-PID1 typically 10000-15000 kB)\n' \
    "$(awk '/VmRSS/{print $2" "$3}' /proc/1/status)"
  printf 'processes:   %s\n' "$(ps -e --no-headers | wc -l)"
  printf 'threads:     %s\n\n' "$(ps -eLf --no-headers | wc -l)"

  printf '### KERNEL → USERSPACE TIMELINE (dmesg, seconds since power-on)\n'
  $SUDO dmesg 2>/dev/null | grep -iE \
    'Freeing unused kernel image \(initmem\)|switch_root|Run /sbin/init|crng init done|EXT4-fs.*mounted|Btrfs loaded' \
    | head -12
  KFREE="$($SUDO dmesg 2>/dev/null | awk -F'[][]' '/Freeing unused kernel image \(initmem\)/{print $2; exit}' | tr -d ' ')"
  printf 'kernel-init-freed at: %ss (kernel hardware init essentially done)\n\n' "${KFREE:-?}"

  printf '### SCHEMA-INIT TIMING (kernel→PID1, then per-service time-to-stable)\n'
  $SCTL timing 2>/dev/null
  printf '\n'

  printf '### SCHEMA-INIT STATUS (pid / state / restarts)\n'
  $SCTL status 2>/dev/null
  printf '\n'

  printf '### CGROUP RESOURCE USE (schema-init managed services)\n'
  if [ -d /sys/fs/cgroup/schema-init ]; then
    for c in /sys/fs/cgroup/schema-init/*/; do
      [ -d "$c" ] || continue
      n="$(basename "$c")"
      cpu="$(awk '/usage_usec/{print $2}' "$c/cpu.stat" 2>/dev/null)"
      mem="$(cat "$c/memory.current" 2>/dev/null)"
      printf '  %-22s cpu_usec=%-12s mem_bytes=%s\n' "$n" "${cpu:-?}" "${mem:-?}"
    done
  else
    printf '  (no /sys/fs/cgroup/schema-init — cgroup tree not present)\n'
  fi
  printf '\n'

  printf '### JOURNAL-SINK TAIL (last 15 lines of unified log)\n'
  $SUDO tail -15 /var/log/schema-init/journal.log 2>/dev/null || printf '  (no journal.log)\n'
  printf '\n'

  PP1="$(awk '/PPid/{print $2}' /proc/"$(pgrep -f '[s]chema-journal-sink' | head -1)"/status 2>/dev/null)"
  printf '### HEALTH CHECKS\n'
  printf 'ctl socket:        %s\n' "$([ -S /run/schema-init.sock ] && echo present || echo MISSING)"
  printf 'journal-sink PPid: %s (1 = schema-init-supervised)\n' "${PP1:-absent}"
  WD_N="$(pgrep -c -f 'schema-plasma-watchdog\.sh' 2>/dev/null)"
  printf 'plasma watchdog:   %s instance(s) [standalone]\n' "${WD_N:-0}"
  printf '\n'
  printf 'full report: %s\n' "$REPORT"
} | tee "$REPORT"

# --- append one CSV row for cross-boot trend tracking ---
TIMING="$($SCTL timing 2>/dev/null)"
K2P="$(printf '%s\n' "$TIMING" | awk -F: '/kernel/{gsub(/[^0-9.]/,"",$2); print $2}')"
# time-to-login: sddm reaching stable is what a boot actually "feels" like
SDDM="$(printf '%s\n' "$TIMING" | awk '$1=="sddm"{gsub(/[^0-9.]/,"",$2); print $2}')"
# slowest service on the rail, and how many services reported
SLOW="$(printf '%s\n' "$TIMING" | awk '/^  /{v=$2; gsub(/[^0-9.]/,"",v); if (v+0>m){m=v+0;n=$1}} END{printf "%s,%s", (m?m:""), (n?n:"")}')"
SVCN="$(printf '%s\n' "$TIMING" | awk '/^  /{c++} END{print c+0}')"
NPROC="$(ps -e --no-headers | wc -l)"
RSS1="$(awk '/VmRSS/{print $2}' /proc/1/status)"
if [ ! -f "$CSV" ]; then
  printf 'timestamp,booted_at,kernel_s,sddm_s,slowest_s,slowest_svc,svc_count,pid1_rss_kb,nproc\n' > "$CSV"
fi

PREV="$(awk -F, 'END{print}' "$CSV" 2>/dev/null)"
printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
  "$STAMP" "$(uptime -s)" "${K2P:-}" "${SDDM:-}" "$SLOW" "$SVCN" "${RSS1:-}" "$NPROC" >> "$CSV"

printf '\n### BOOT DELTA (vs previous recorded boot)\n'
printf '%s\n' "$PREV" | awk -F, -v k="${K2P:-0}" -v s="${SDDM:-0}" '
  /^timestamp/ || NF < 5 { print "  (no previous boot recorded — this row is the baseline)"; exit }
  { printf "  vs %s\n", $1
    printf "  kernel to PID1:  %7.3fs -> %7.3fs  (%+.3fs)\n", $3, k, k-$3
    if ($4 != "" && s+0 > 0)
      printf "  time to login:   %7.3fs -> %7.3fs  (%+.3fs)\n", $4, s, s-$4
    else
      printf "  time to login:   (not recorded previously) -> %7.3fs\n", s }' \
  | tee -a "$REPORT"
printf 'CSV row appended: %s\n' "$CSV"
