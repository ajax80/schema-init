# Kernel-surface ns knobs (audit 4.5 part 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** protect_kernel_tunables=, protect_kernel_modules=, protect_control_groups=, private_devices=, protect_hostname= as native .svc keys, mapped by the importer.

**Architecture:** One `uint8_t ns_kernel` bitmask in service_t, passed as a new last-but-two argument to `apply_mount_ns` (ns.c), which builds the extra mounts in the same mount pass. Bounding-set drops happen in `service_apply_hardening` from `ns_kernel_cap_drop()`. Importer maps the five systemd directives natively.

**Tech Stack:** C (static PID 1, no new libs), Python importer + unittest.

**Spec:** `docs/superpowers/specs/2026-11-16-kernel-surface-ns-design.md`

## Global Constraints

- All five keys take only "0" / "1" (parse_ns_bool); anything else is a load error.
- None joins the hardening default; no HARD_* bit.
- No new dependencies; everything in the forked child before caps drop.
- Missing paths (ENOENT) are skipped, as bind_attr/mount_tmpfs do today.
- An explicit knob hiding the service's exec or ready_path refuses the load.

## Review Focus

1. Device nodes in the private /dev must work (no nodev on their mounts): writing /dev/null succeeds. Test in Task 2.
2. A host where /dev/mqueue or /dev/log is absent: private_devices still succeeds. Test: Task 2 binds through a helper that treats ENOENT as skip; covered by running on a userns where /dev/log may be missing (assert only "if host has it").
3. keep_caps= naming CAP_SYS_MODULE with protect_kernel_modules=1: the cap is still gone from the bounding set. Test in Task 3.
4. ambient_caps= naming a cap a knob drops: refuse the load (it could never be raised). Test in Task 3.
5. rw_path= under /sys with protect_kernel_tunables=1 makes that path writable again (rw pass stays last). Test in Task 2.

---

### Task 1: Key parsing, struct field, ns_hides

**Files:**
- Modify: `ns.h` (flag defines), `service.h:489-493` (field), `service.c:448-470` (parse_ns_field), `service.c:704-710` (hardening_finalize)
- Test: `tests/test_mountns_parse.c`

**Interfaces:**
- Produces in `ns.h`:
  `#define NS_K_TUNABLES 0x01`, `NS_K_MODULES 0x02`, `NS_K_CGROUPS 0x04`, `NS_K_DEVICES 0x08`, `NS_K_HOSTNAME 0x10`
- Produces in `service.h`: `uint8_t ns_kernel; /* NS_K_* (ns.h) */`

- [ ] **Step 1: Failing tests** in test_mountns_parse.c (its write_svc helper):
  - each of the five keys `=1` sets its bit; `=0` leaves ns_kernel 0
  - `protect_hostname=2` → service fails to load
  - `protect_kernel_modules=1` + `exec=/usr/lib/modules/x/helper` → not loaded
  - `private_devices=1` + `ready_path=/dev/schema-ready` → not loaded
  - `private_devices=1` + `ready_path=/run/x` → loads
- [ ] **Step 2:** `make test` → mountns fails.
- [ ] **Step 3:** parse_ns_field: table of {key, bit}; parse_ns_bool into a temp, set/clear the bit. hardening_finalize: ns_hides with bit 0 (explicit-only) for modules over `{"/usr/lib/modules/", "/lib/modules/"}` and devices over `{"/dev/"}` — pass a temp uint8_t holding the bit's state, since ns_hides takes a knob pointer.
- [ ] **Step 4:** `make test` → passes.
- [ ] **Step 5:** Commit.

### Task 2: Mounts in apply_mount_ns

**Files:**
- Modify: `ns.c`, `ns.h` (signature), `service.c:714` (caller), `tests/test_mountns_rw.c` (all 5 callers pass kernel=0)
- Create: `tests/test_kernel_ns.c`; Makefile test line next to mountns-rw (links ns.c caps.c)

**Interfaces:**
- `int apply_mount_ns(const char *name, uint8_t private_tmp, uint8_t protect_system, uint8_t protect_home, uint8_t kernel, const char *const *rw, int n_rw, const char **step);`
  early return now also requires `!kernel`; `step` names: "protect_kernel_tunables", "protect_kernel_modules", "protect_control_groups", "private_devices", "protect_hostname".

Order after protect_home, before rw_path: hostname (unshare CLONE_NEWUTS), tunables (bind_attr ro over `/proc/sys /sys /proc/sysrq-trigger /proc/latency_stats /proc/acpi /proc/apm /proc/asound /proc/bus /proc/fs /proc/irq /proc/scsi`), cgroups (bind_attr ro `/sys/fs/cgroup`), modules (ro tmpfs `mode=0755` over `/usr/lib/modules`, `/lib/modules`), devices.

private_devices algorithm:
1. Before covering /dev: O_PATH|O_CLOEXEC fds of host `/dev/{null,zero,full,random,urandom,tty,shm,mqueue,log}` (ENOENT → -1, skipped later); tty gid = fstat of /dev/tty fd (5 if missing).
2. tmpfs on /dev, MS_NOSUID|MS_NOEXEC, `mode=0755,size=4m`.
3. For each held fd: fstat it; S_ISDIR → mkdir 0755, else create an empty 0666 file and bind `/proc/self/fd/N` onto it with MS_BIND only (no nodev: these are device nodes).
4. mkdir /dev/pts; mount devpts `newinstance,ptmxmode=0666,mode=0620,gid=<tty gid>` MS_NOSUID|MS_NOEXEC; on EINVAL (tty gid unmapped, e.g. the userns test) retry without gid=; symlink `pts/ptmx` → /dev/ptmx.
5. Symlinks: fd→/proc/self/fd, stdin/stdout/stderr→/proc/self/fd/0,1,2.
6. Close every held fd on success and on each error path.

- [ ] **Step 1: Failing test** tests/test_kernel_ns.c. Root, else child enters a user namespace (unshare CLONE_NEWUSER|CLONE_NEWNS, map own uid/gid to 0, setgroups deny); EPERM → print `SKIP test_kernel_ns (no userns)` and pass. One forked child per case:
  - tunables: `access(p, W_OK)` gives EROFS for /proc/sys/kernel/hostname, /sys/kernel, /proc/sysrq-trigger
  - tunables + rw `/sys/kernel` (via rw list): access no longer EROFS
  - cgroups only: /sys/fs/cgroup EROFS; /proc/sys/kernel/hostname errno != EROFS
  - modules: /usr/lib/modules readdir yields only . and .. (skip if host lacks the dir)
  - devices: write(open("/dev/null")) == 1; stat("/dev/kmsg") ENOENT; posix_openpt+ptsname begins "/dev/pts/"; readlink("/dev/fd") == "/proc/self/fd"; statvfs("/dev") has ST_NOEXEC; /dev/log is S_ISSOCK when host has it
  - hostname: sethostname("schema-kns-test") succeeds in child; parent's gethostname unchanged
  - all five + PROTECT_SYSTEM_FULL + private_tmp together: returns 0
- [ ] **Step 2:** `make test` → compile error (signature).
- [ ] **Step 3:** Implement; update both callers.
- [ ] **Step 4:** `make test` (CI path, non-root) and `sudo $(TESTDIR)/kns` → pass.
- [ ] **Step 5:** Commit.

### Task 3: Bounding-set drops

**Files:**
- Modify: `ns.h`/`ns.c` (`uint64_t ns_kernel_cap_drop(uint8_t kernel)`), `caps.h`/`caps.c` (`int drop_bounding(uint64_t mask)`), `service.c` service_apply_hardening + hardening_finalize
- Test: `tests/test_kernel_ns.c`, `tests/test_mountns_parse.c`

**Interfaces:**
- `ns_kernel_cap_drop`: MODULES → CAP_SYS_MODULE; DEVICES → CAP_MKNOD | CAP_SYS_RAWIO.
- `drop_bounding(mask)`: PR_CAPBSET_DROP each set bit; 0 / -1 errno.
- service_apply_hardening: cap_restrict → `apply_capabilities((keep|ambient) & ~drop)`; else if (drop) `drop_bounding(drop)`; failure prints `HARDENING FAILED for %s: kernel caps: %d`.

- [ ] **Step 1: Failing tests:**
  - kns child: drop_bounding(ns_kernel_cap_drop(NS_K_MODULES|NS_K_DEVICES)), then PR_CAPBSET_READ is 0 for SYS_MODULE, MKNOD, SYS_RAWIO and 1 for CAP_NET_ADMIN
  - parse: `ambient_caps=CAP_SYS_MODULE` + `protect_kernel_modules=1` → not loaded; `keep_caps=CAP_SYS_MODULE` + `protect_kernel_modules=1` → loads
- [ ] **Step 2:** fail. **Step 3:** implement (conflict message: `%s: ambient_caps holds a cap %s drops — not loaded`). **Step 4:** pass. **Step 5:** Commit.

### Task 4: Status, importer, docs

**Files:**
- Modify: `engine.c:2342` (after protect_home: one line per set knob, `"  %-15s 1\n"` style widened to `%-24s` for these), `distros/fedora-installer/migrate/schema-import.py` (_hardening, _UNENFORCED, _USER_NO_HARDEN), `docs/svc-reference.md`, `tests/test_import_units.py`, `tests/test_import_user.py`

- [ ] **Step 1: Failing importer tests:**
  - `ProtectKernelTunables=yes` → `protect_kernel_tunables=1` and no "not enforced: ProtectKernelTunables" warning; same for the other four
  - `PrivateDevices=no` → no private_devices line
  - `ProtectHostname=bogus` → warning `ProtectHostname=bogus unrecognised, not applied`, no line
  - user mode: `PrivateDevices=yes` → line dropped with the "schema-user cannot set up a mount namespace" warning
  - existing test at test_import_units.py:1142 switches its example to ProtectClock
- [ ] **Step 2:** fail. **Step 3:** implement — emit `key=1` only when true (keeps old imports byte-identical); remove the five from _UNENFORCED; add them to _USER_NO_HARDEN. svc-reference: one entry per key, plus the /dev/log limit and "device_allow= not yet". **Step 4:** `make test` + `python3 -m pytest tests/test_import_units.py tests/test_import_user.py -q` pass. **Step 5:** Commit.

### Task 5: Draft PR, review, live test

- [ ] Push, `gh pr create --draft`; `/code-review` once; fix.
- [ ] Live on blakbox PID 1: `sudo schema-ctl reexec <branch build>`; test .svc in /etc/schema-init/services with all five + a script that records: hostname set, /proc/sys write, /dev listing, `grep CapBnd /proc/self/status`, `logger` delivered; `schema-ctl add`; check; remove; bare `sudo schema-ctl reexec`; 67 healthy.
- [ ] Merge after his go.
