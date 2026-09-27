# Service hardening — default-flip (opt-in per host)

Status: design, 2026-09-26. Follows Phase 1 (no_new_privs, keep_caps) and
Phase 3 (private_tmp, protect_system, protect_home).

## Problem

"Unset ⇒ hardened" as a global binary change breaks ~45 of the fleet's 77
distinct services (audit 2026-09-26, 144 .svc across 5 hosts):

- `no_new_privs` is inherited by every descendant: anything that spawns a
  login (sshd, getty, sddm, vnc, seatd, polkitd's setuid agent helper,
  udisks2, docker) loses sudo/pkexec.
- Any mount-ns knob gives the service a private (MS_SLAVE) mount namespace:
  mounts it makes never reach the host (every mount-* oneshot, nfs-server,
  udisks2, docker/containerd, x11-tmpfiles).
- `protect_system` breaks /boot and /etc writers (schema-bootok,
  udev-flip healthcheck, schema-doctor, logrotate); `protect_home` breaks
  daemons that serve /home (smbd, nfs-mountd on blakbox).

COPR users and units produced by `schema-import` would change behaviour on a
plain `dnf upgrade`. That is not acceptable.

## Design

1. **Tri-state fields.** Each of `no_new_privs`, `private_tmp`,
   `protect_system`, `protect_home` records whether it was set explicitly.
   An explicit `=0` always wins. Both .svc parsers (`services_load`,
   `service_load_one`).

2. **Per-host switch, default OFF.** PID 1 reads
   `/etc/schema-init/hardening-default` once at boot. Absent or not `on` ⇒
   today's behaviour, bit for bit. `on` ⇒ every knob a service leaves unset
   takes: `no_new_privs=1`, `protect_system=1` (not `full`),
   `protect_home=1`, `private_tmp=1`. `keep_caps` is never defaulted.
   Kernel cmdline `schema.hardening_default=0` forces OFF (rescue).
   Read at boot only — changing it takes a reboot, same as .svc edits.

3. **Defaulted knobs never refuse a load.** If a *defaulted* mount-ns knob
   would hide the service's exec or ready_path (`ns_conflict`), that knob is
   dropped for that service and logged. An *explicit* conflicting knob still
   refuses the load, as today.

4. **schema-import emits all four fields explicitly**, mapped from the unit:
   `NoNewPrivileges=yes` → 1, `PrivateTmp=yes` → 1,
   `ProtectSystem=yes` → 1 / `full|strict` → full, `ProtectHome=yes` → 1;
   everything else (including `ProtectHome=read-only|tmpfs`) → 0. An imported
   unit therefore means the same thing whether the switch is on or off.

5. **Opt-in rollout.** No host turns the switch on until every .svc on it
   carries an explicit value for each knob it cannot take. Order:
   code (switch off everywhere) → schema-import → fleet .svc annotation from
   the audit → switch on one host at a time, reboot-verified → ISO/wizard
   turns it on for fresh installs whose rail is fully annotated.

## Tests (vmtest, red/green)

- switch absent: unset svc unhardened (control, anti-false-green)
- switch `on`: unset svc has NoNewPrivs 1, own mnt ns, /usr RO, /home empty
- switch `on` + explicit `=0` per knob: that knob off
- switch `on` + exec under /home: loads, protect_home dropped, logged
- switch `on` + `schema.hardening_default=0` on cmdline: unhardened
- schema-import: unit with/without directives → four explicit fields

## Separate, no code change

Hand-harden the easy-win daemons explicitly now (like chronyd, wsdd):
avahi, bluetoothd, rpcbind, upower, wpa_supplicant, tailscaled
(`protect_system=1`, resolv.conf), ollama, schema-systemd1, kodi-autolib,
heartbeat, logger; smbd and nfs-mountd take `protect_system` + `private_tmp`
but not `protect_home`. Each: edit .svc before a reboot, verify after.
