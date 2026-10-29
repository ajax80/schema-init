# Pi Service Kit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `apt install schema-init && sudo schema-pi enable` on a stock Pi OS trixie box boots schema-init with ssh, network, clock, logging and (when installed) Pi-hole, with no hand-written `.svc`.

**Architecture:** A small shipped core kit covers early boot, udev, D-Bus, clock and logging. `schema-pi enable` installs the kit, then runs `schema-import --enabled` for everything systemd had enabled. It then wires the network service to coldplug and writes the boot-OK list. The engine gains `ambient_caps=` and multi-hour `on_calendar=`; the importer learns `AmbientCapabilities=`, `RuntimeDirectory=` and multi-hour `OnCalendar=`.

**Tech Stack:** C (PID 1 engine: `service.c`, `service.h`), POSIX sh (dash) scripts, Python 3 (`schema-import.py`), Debian packaging, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-28-pi-service-kit-design.md` (approved 10-28). Read it before any task.

## Global Constraints

- Target: Raspberry Pi OS / Debian trixie, arm64 and armhf (ARMv6). Fedora output must not change for units that already import correctly; existing importer tests must pass.
- Scripts are POSIX sh, dash-compatible. `schema-init-pi` stays static C, fixed buffers.
- Kit files ship as `services/<name>.svc.example` → `/usr/share/schema-init/`, scripts → `/usr/libexec/schema-init/`. Nothing in `/usr/local`.
- Enable never overwrites an existing `.svc` in `/etc/schema-init/services/`.
- Never add `rsyslog.svc`. `journal-sink` owns `/dev/log`.
- Boot-OK list: `/etc/schema-init/pi-boot-required`, one service name per line, `#` comments and blanks ignored. **Missing file = today's rule** (every service FUNDAMENTAL/PERFECT).
- Counted in boot OK: `udev`, `udev-trigger`, `dbus`, `pi-early`, ssh (`ssh` or `sshd`), the detected network service, `pihole-FTL` when its `.svc` exists, `unbound` only when both `pihole-FTL.svc` and `unbound.svc` exist. **Never** counted: `pi-timesync`, `journal-sink`, `pi-boot-firmware`, every other import.
- Network detection order (first enabled wins): NetworkManager, systemd-networkd, networking, dhcpcd, wpa_supplicant.
- Kit service names: `pi-early`, `udev`, `udev-trigger`, `dbus`, `pi-timesync`, `journal-sink` (plus the existing `pi-boot-firmware`, `pi-bootok`, `pi-deadline`). `dbus` must be named exactly `dbus`: that is the name the importer's implicit bus-daemon `dep=` resolves to.
- Clock: HTTP `Date` header from `http://1.1.1.1/` then `http://8.8.8.8/`, needs no DNS, never fails the boot.
- No docstrings, only why-comments. Focused commits.
- DANGER for every implementer: blakbox runs schema-init as PID 1. Never run `schema-pi`, `schema-pi-bootok`, `schema-pi-deadline`, `schema-ctl` or `schema-import` (without `--dry-run` and redirected `SCHEMA_SVC_DIR`/`SCHEMA_STATE_DIR`/`SCHEMA_BUS_OVERRIDE_DIR`/`MIGRATE_ROOT`) on blakbox. Use fake roots and PATH shims.

## Review Focus

1. **Wi-Fi-only Pi:** `wlan0` appears after udev coldplug. The network service must wait for `udev-trigger` or the boot fails. Task 5 test: the detected network `.svc` gets a drop-in with `dep=udev-trigger`.
2. **A Pi already enabled before this PR (BlakPi):** upgrade must not change its boot gate. Task 6 test: no `pi-boot-required` gives today's every-service rule.
3. **ssh from Debian's unit:** `sshd -D` needs `/run/sshd`, so the imported `ssh.svc` must create it. Task 3 test: a unit with `RuntimeDirectory=sshd` + `RuntimeDirectoryMode=0755` emits a root `exec_pre` that creates `/run/sshd` 0755.
4. **Pi-hole as `user=pihole`:** port 53 needs `CAP_NET_BIND_SERVICE` after the uid drop. Task 1 test (root only): a child with `ambient_caps=CAP_NET_BIND_SERVICE`, `user=nobody` has bit 10 in `CapAmb` and `CapEff`.
5. **A user's own `.svc` with the same name as an import or kit file:** it must survive enable untouched. Task 5 test: a pre-existing `ssh.svc` and `dbus.svc` keep their bytes after enable.

---

### Task 1: Engine `ambient_caps=`

**Files:**
- Modify: `service.c` (key parse near `keep_caps` ~2331; child setup around the uid drop ~1488-1512), `service.h` (field `uint64_t cap_ambient_mask` in `service_t`)
- Test: `tests/test_cap_parse.c` (parse), new `tests/test_ambient_caps.c` (root-only runtime)

**Interfaces:**
- Produces: svc key `ambient_caps=CAP_A,CAP_B` (same list syntax as `keep_caps=`, parsed with the existing `parse_cap_list`). `static int apply_ambient_caps(uint64_t mask, uid_t uid, gid_t gid, const char *user)` in `service.c`, also exposed via a test-only header path the same way `apply_capabilities` is tested (follow the existing test's pattern).

- [ ] **Step 1: Write failing parse test** in `tests/test_cap_parse.c`: parsing `ambient_caps=CAP_NET_BIND_SERVICE,CAP_NET_RAW` sets `cap_ambient_mask == (1ULL<<10)|(1ULL<<13)`; `ambient_caps=CAP_BOGUS` makes the svc load fail.
- [ ] **Step 2: Write failing runtime test** `tests/test_ambient_caps.c`: skip (exit 0, print `skip: not root`) unless euid 0. Fork; the child calls `apply_ambient_caps((1ULL<<10), <nobody uid>, <nobody gid>, "nobody")`, then reads `/proc/self/status`. Assert `CapAmb` and `CapEff` both have bit 10 and uid is nobody's.
- [ ] **Step 3: Run both and verify they fail** (`make test` target lines added next to `test_cap_parse`).
- [ ] **Step 4: Implement.** Order, while root: `prctl(PR_SET_KEEPCAPS, 1)`; set inheritable = mask via `capset`; `initgroups`/`setgid`/`setuid`; re-set effective+permitted+inheritable = mask with `capset`; then `prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0)` for each bit. Wire it into the uid-drop path only when `cap_ambient_mask != 0` and `user=` is set; otherwise behaviour is unchanged. A failure exits 126 with `[schema-init] AMBIENT CAPS FAILED for <svc>: <step>: <errno>`. Why-comment: ambient caps are cleared on the uid change, so they are raised after it.
- [ ] **Step 5: Run `make test` (parse test passes) and `sudo` the runtime test binary once by hand** (not as PID 1 work, a plain test binary). Expected: PASS.
- [ ] **Step 6: Commit** `engine: ambient_caps= raises ambient capabilities after the uid drop`.

### Task 2: Multi-hour `on_calendar=`

**Files:**
- Modify: `service.h:189-240` (`parse_calendar_fields`, `calendar_next_after`, `calendar_recent_at_or_before`), `service.c:~1934` (stores `timer_cal_hour`), `engine.c:123,160` (callers)
- Test: `tests/test_calendar.c`

**Interfaces:**
- Produces: `int parse_calendar_fields(const char *val, uint32_t *hour_mask, int *min, int *dow, int *dom)`. Hours are a comma list (`6,18:00`, `Mon 6,18:00`), each 0-23, mask bit h set. A single hour gives a one-bit mask. `time_t calendar_next_after(uint32_t hour_mask, int min, int dow, int dom, time_t after)` and the same change to `calendar_recent_at_or_before`. The `service_t` field becomes `uint32_t timer_cal_hours` (0 = no calendar, replacing `timer_cal_hour = -1`).

- [ ] **Step 1: Failing tests** in `tests/test_calendar.c`:
  - `6,18:00` → mask `(1<<6)|(1<<18)`, min 0;
  - `24,1:00`, `6,,18:00` and `6,18` → -1;
  - with TZ=UTC, next after 2026-10-28 07:00 is 18:00 the same day, and next after 19:00 is 06:00 the next day;
  - every existing single-hour case still passes.
- [ ] **Step 2: Run and verify they fail.** `make test`.
- [ ] **Step 3: Implement.** Scan hours within each day in ascending order. Keep the DST self-correction of the current day walk.
- [ ] **Step 4: Run `make test`.** Expected: PASS, including `persistent=1` catch-up tests that use `calendar_recent_at_or_before`.
- [ ] **Step 5: Commit** `timers: on_calendar= takes a comma list of hours`.

### Task 3: Importer — ambient caps, runtime dirs, multi-hour calendar, MIGRATE_ROOT

**Files:**
- Modify: `distros/fedora-installer/migrate/schema-import.py` (`unit_to_svc` ~875, `_calendar` ~461)
- Test: `tests/test_import_units.py`

**Interfaces:**
- Consumes: Task 1 key `ambient_caps=`; Task 2 `on_calendar=H1,H2:MM`.
- Produces: imported units carry `ambient_caps=` and `exec_pre=+/usr/bin/install -d -m <mode> [-o <user> -g <group>] /run/<dir>`.

- [ ] **Step 1: Failing tests:**
  - `AmbientCapabilities=CAP_NET_BIND_SERVICE CAP_NET_RAW` with `User=pihole` gives the line `ambient_caps=CAP_NET_BIND_SERVICE,CAP_NET_RAW`;
  - `RuntimeDirectory=sshd` with `RuntimeDirectoryMode=0755` gives `exec_pre=+/usr/bin/install -d -m 0755 /run/sshd`, ordered before the unit's own `ExecStartPre` lines;
  - with `User=foo`, the same line carries `-o foo -g foo`, or the unit's `Group=` if set;
  - several space-separated dirs each get a line;
  - `OnCalendar=*-*-* 6,18:00` gives `on_calendar=6,18:00` instead of a skip;
  - an existing Fedora fixture without these keys produces byte-identical output.
- [ ] **Step 2: Run** `python3 -m pytest tests/test_import_units.py -q` (or how the Makefile runs it). Expected: the new tests FAIL.
- [ ] **Step 3: Implement** in `unit_to_svc`/`_calendar`. The `RuntimeDirectory=` default mode is `0755`. Why-comment: systemd creates these, and schema-init does not.
- [ ] **Step 4: Run** the importer tests plus `tests/test_migrate_import_enabled.py`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-import: AmbientCapabilities=, RuntimeDirectory=, multi-hour OnCalendar=`.

### Task 4: Core kit files + packaging

**Files:**
- Create: `services/pi-early.svc.example`, `services/udev.svc.example`, `services/udev-trigger.svc.example`, `services/dbus.svc.example`, `services/pi-timesync.svc.example`; `scripts/schema-pi-early`, `scripts/schema-pi-timesync`, `scripts/schema-pi-udev-trigger`, `scripts/schema-pi-dbus`
- Modify: `services/journal-sink.svc.example:2` (`exec=/usr/bin/schema-journal-sink`), `debian/schema-init.install` (scripts → `usr/libexec/schema-init`; `schema-import` → `usr/libexec/schema-init/schema-import` + symlink `usr/bin/schema-import` via `debian/schema-init.links`), `debian/control` (`Depends:` adds `python3`)
- Test: new `tests/test_pi_kit.sh` (added to `make test`)

**Interfaces:**
- Produces: the kit names in Global Constraints and their `dep=` graph:
  - `pi-early` (oneshot, needs_root) has no deps;
  - `udev` deps on `pi-early`; `udev-trigger` (oneshot) on `udev`;
  - `dbus` deps on `pi-early`, with `ready_path=/run/dbus/system_bus_socket`;
  - `journal-sink` deps on `pi-early`;
  - `pi-timesync` (long-running, `oneshot=0`) has no deps, since it waits for a default route itself.
  
  Paths: `/lib/systemd/systemd-udevd`, `/usr/bin/dbus-daemon --system --nofork`.
- `schema-pi-early`: lo up (127.0.0.1/8, ::1/128), `hostname "$(head -n 1 /etc/hostname)"`, `swapon -a`; always exit 0. (`/dev/shm` is PID 1's, and `/boot/firmware` is `pi-boot-firmware`'s.)
- `schema-pi-timesync`:
  - loop: wait for a default route, then try the HTTP Date from 1.1.1.1 and then 8.8.8.8;
  - on success set the clock, then sleep `${SCHEMA_PI_RESYNC:-21600}` and repeat;
  - on failure sleep `${SCHEMA_PI_RETRY:-30}` and retry;
  - never exits.
  
  Why-comment: no RTC, `systemd-timesyncd` is reclaimed by the importer, and the clock must not need DNS. Overridable for tests: `SCHEMA_PI_ROOT`, `SCHEMA_PI_CURL`, `SCHEMA_PI_DATE`.

- [ ] **Step 1: Failing tests** in `tests/test_pi_kit.sh`, using PATH shims for `ip`, `hostname`, `swapon`, `curl` and `date`:
  - `schema-pi-early` calls `ip link set lo up` and `hostname <name from fake /etc/hostname>`, and exits 0 even when the shims fail;
  - `schema-pi-timesync` with a curl shim that fails on 1.1.1.1 and returns a `Date:` header on 8.8.8.8 calls `date -s "<that value>"` (run with `SCHEMA_PI_RESYNC=0` and a one-iteration guard such as `SCHEMA_PI_ONCE=1`);
  - with both failing, it sleeps the retry interval and does not exit non-zero;
  - every new `.svc.example` parses: name= matches the file, and every dep= names a kit file;
  - `journal-sink.svc.example` execs `/usr/bin/schema-journal-sink`.
- [ ] **Step 2: Run** `sh tests/test_pi_kit.sh`. Expected: FAIL.
- [ ] **Step 3: Implement the scripts and `.svc.example` files**, porting from BlakPi's working hand set (copy in the session scratchpad `/tmp/claude-1000/-/965c597f-ece5-4845-86fe-0776dc14a791/scratchpad/blakpi/`, live on BlakPi 192.168.8.223). Debian packaging: install, links, Depends.
- [ ] **Step 4: Run `make test`**, then a local trixie container deb build (`dpkg-buildpackage -b -us -uc` as in Task 5 of the #442 plan), and check that `dpkg -c` lists every new file and `/usr/bin/schema-import`. Expected: PASS, no `/usr/local`.
- [ ] **Step 5: Commit** `pi: core service kit + schema-import in the deb`.

### Task 5: `schema-pi enable` drives kit + import

**Files:**
- Modify: `scripts/schema-pi` (enable)
- Test: `tests/test_schema_pi.sh`

**Interfaces:**
- Consumes: Task 4 kit names; `schema-import` at `${SCHEMA_IMPORT:-/usr/libexec/schema-init/schema-import}`, called with `MIGRATE_ROOT=$R` and `SCHEMA_SVC_DIR=$R/etc/schema-init/services`.
- Produces: `/etc/schema-init/pi-boot-required` (format in Global Constraints); the drop-in `$SVC/<net>.svc.d/pi-kit.conf` containing `dep=udev-trigger` and `dep=dbus`; on stdout, `schema-pi: imported N, skipped M` followed by one line per skip, `skip <unit>: <reason>`, taken from the importer's output.
- Enable order:
  1. mount `/boot/firmware` (existing);
  2. install the kit `.svc.example` files that are absent;
  3. run `schema-import --enabled`;
  4. detect the network service in the Global Constraints order, from `$R/etc/systemd/system/*.wants/`;
  5. write the drop-in;
  6. require an ssh service (`ssh.svc` or `sshd.svc`), else die `no ssh service after import: schema-init would boot without ssh`;
  7. write `pi-boot-required`;
  8. existing known-good seed and cmdline edit.

  The old precondition "sshd.svc must exist before enable" is removed.

- [ ] **Step 1: Failing tests:**
  - **Kit install:** a fake root with `ssh.service` and `NetworkManager.service` enabled, plus an importer shim that writes `ssh.svc` and `NetworkManager.svc` and prints one `skip` line. After enable, every kit `.svc` is installed and `NetworkManager.svc.d/pi-kit.conf` has both deps.
  - **pi-boot-required:** it contains exactly `udev`, `udev-trigger`, `dbus`, `pi-early`, `ssh` and `NetworkManager`. Stdout lists the skip.
  - **Pi-hole:** when the shim also writes `pihole-FTL.svc` and `unbound.svc`, both are in the list. With `pihole-FTL.svc` alone, only `pihole-FTL` is.
  - **Wi-Fi only:** with only `wpa_supplicant.service` enabled, the network service is `wpa_supplicant`.
  - **No ssh:** if the importer writes no ssh service, enable dies with the message above and `cmdline.txt` is unchanged.
  - **User files win (Review Focus 5):** pre-existing `ssh.svc` and `dbus.svc` keep their bytes.
  - **Re-run:** re-running enable is idempotent. The list is rewritten identically and the drop-in is not duplicated.
- [ ] **Step 2: Run** `sh tests/test_schema_pi.sh`. Expected: FAIL.
- [ ] **Step 3: Implement** in `scripts/schema-pi`.
- [ ] **Step 4: Run** `make test`. Expected: PASS.
- [ ] **Step 5: Commit** `schema-pi enable: install the kit, import systemd's enabled units, write the boot-OK list`.

### Task 6: bootok reads the boot-OK list

**Files:**
- Modify: `scripts/schema-pi-bootok` (`blockers()`, `promote()`), `scripts/schema-pi` (status)
- Test: `tests/test_pi_bootok.sh`, `tests/test_schema_pi.sh`

**Interfaces:**
- Consumes: `pi-boot-required` from Task 5.
- Produces:
  - `blockers()` reads `$KG/pi-boot-required` on a known-good boot (the script runs from `$KG`), else `$R/etc/schema-init/pi-boot-required`.
  - When the file exists, only the services it lists can block, and `ssh` and `sshd` both satisfy the ssh requirement. A listed service that is not loaded counts as `MISSING`.
  - When the file is missing, today's rule applies unchanged (Review Focus 2).
  - `promote()` copies the list into `$KG.new/pi-boot-required` when it exists.
  - `schema-pi status` adds `not up: <svc>=<state> ...` for services that are not FUNDAMENTAL/PERFECT.

- [ ] **Step 1: Failing tests:**
  - with a list naming `ssh` and a fake `schema-ctl status --kv` where `avahi-daemon=EXCISED` and the rest are FUNDAMENTAL, bootok confirms;
  - with `ssh=DORMANT` it waits;
  - with no list file and `avahi-daemon=EXCISED`, it waits (today's rule);
  - with `sshd` listed and only `ssh` loaded, the ssh requirement is satisfied;
  - after `--promote`, `known-good/pi-boot-required` matches the source;
  - on a known-good boot, the known-good copy is used: a differing `/etc` list is ignored;
  - status prints `not up: avahi-daemon=EXCISED`.
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test`. Expected: PASS.
- [ ] **Step 5: Commit** `pi-bootok: boot OK counts the required list; status shows what's not up`.

### Task 7: Docs + CI Node 24

**Files:**
- Modify: `distros/raspberry-pi/README.md` (the "Services first" section becomes "What enable sets up": the kit, the import, the boot-OK list and how to edit it, the skip report); `.github/workflows/deb.yml`; schema-init-apt `.github/workflows/publish.yml` (clone at `/tmp/claude-1000/-/965c597f-ece5-4845-86fe-0776dc14a791/scratchpad/schema-init-apt`)

- [ ] **Step 1:** Look up each action's current major release (`gh api repos/actions/<name>/releases/latest`) for checkout, upload-artifact, download-artifact, cache (restore/save), upload-pages-artifact and deploy-pages. Pin each to the major whose `action.yml` declares `using: node24`, checking with `gh api repos/actions/<name>/contents/action.yml?ref=<tag>`.
- [ ] **Step 2:** Bump the pins in `deb.yml`. Push the branch and confirm the run has no Node 20 deprecation annotation (`gh run view <id> --json annotations` or the run page).
- [ ] **Step 3:** Bump `publish.yml` in schema-init-apt **locally only**. Do not push. The controller asks Jonathan before that push.
- [ ] **Step 4:** Update the README. Commit `docs/ci: Pi kit README; actions on Node 24`.

### Task 8: BlakPi onto the kit (hardware)

BlakPi = 192.168.8.223, arm64. Jonathan allows rebooting it freely, including deliberate failed boots. It is the LAN's secondary Pi-hole; RustConn VNC must keep working.

- [ ] **Step 1:** Publish the branch build to dev is **forbidden**: dev is master-only. Instead, copy the branch CI's arm64 deb artifact to BlakPi and run `sudo apt install ./schema-init_*.deb`. Expect a live re-exec with all services adopted.
- [ ] **Step 2:** Back up `/etc/schema-init/services`, `/usr/local/bin/{blakpi-*,schema-*,tailscaled-run.sh}` and `cmdline.txt` to `/root/pre-kit-<date>/`.
- [ ] **Step 3:** Remove the hand set:
  - all `.svc` files except `vnc.svc` (VNC stays hand-written: tigervncserver@ is a template the importer skips);
  - the `/usr/local/bin` hand scripts except `schema-vnc-start.sh`.
  
  Then run `sudo schema-pi enable`. Check:
  - the summary prints;
  - `pi-boot-required` holds `udev`, `udev-trigger`, `dbus`, `pi-early`, `ssh`, `NetworkManager`, `pihole-FTL`, `unbound`;
  - `vnc.svc` is untouched.
- [ ] **Step 4:** Reboot. Pass when:
  - PID 1 is schema-init;
  - boot-ok is confirmed with `attempts=0`;
  - `dig @192.168.8.223 example.com` answers and `doubleclick.net` returns `0.0.0.0`;
  - `tailscale status` shows up;
  - `:5901` answers;
  - `date` is correct;
  - `ls /run/sshd` exists;
  - `grep CapAmb /proc/$(pgrep -x pihole-FTL)/status` is nonzero;
  - `logger test` reaches journal-sink's log.
- [ ] **Step 5:** Failure drill: stop `avahi-daemon` from starting (e.g. `chmod -x` its binary), reboot, and expect boot-ok with avahi listed as `not up`. Restore it.
- [ ] **Step 6:** Record the results in the report and in memory (`project_schema_pi_packaging`). The controller opens the PR as **draft**, then runs one `/code-review`.
