# Pi apt packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship schema-init to Raspberry Pis (armhf ARMv6, arm64) and Debian amd64 as signed .debs from a GitHub Pages apt repo with stable and dev channels, live re-exec on upgrade, and a boot fallback that can never strand a headless Pi.

**Architecture:** A static C wrapper (`schema-init-pi`) is the kernel's `init=`. It counts boot attempts and picks the current engine, the known-good set, or systemd for one boot. Two small supervised services run under schema: `pi-bootok` confirms a healthy boot and promotes the known-good set, and `pi-deadline` reboots after 5 min without confirmation. `schema-pi enable|disable|status` edits `cmdline.txt`. A `debian/` package is built by GitHub Actions per arch. A dispatch to the `ajax80/schema-init-apt` repo merges, signs and deploys the apt repo as a Pages artifact.

**Tech Stack:** C99 (static, same Makefile), POSIX sh, dpkg-buildpackage/debhelper, apt-ftparchive + gpg, GitHub Actions (`ubuntu-24.04-arm`, QEMU user + Raspbian debootstrap), `actions/deploy-pages`.

**Spec:** `docs/superpowers/specs/2026-10-28-pi-apt-packaging-design.md`

## Global Constraints

- Targets: Raspberry Pi OS / Debian 13 trixie only. Arches `armhf` (ARMv6+VFP), `arm64`, `amd64`.
- armhf is built against a Raspbian userland only. Every shipped armhf ELF must pass `readelf -A`: `Tag_CPU_arch: v6` or `v6KZ`. v7 or newer fails the job.
- Nothing installed under `/usr/local`. Helpers go to `/usr/libexec/schema-init`.
- `schema-init` must not depend on `schema-init-daemons`. It does `Depends: systemd`.
- `apt install` never changes PID 1. Only `schema-pi enable` does.
- Re-exec on upgrade runs from a dpkg trigger, never inline in `postinst configure`.
- Boot OK means `sshd` is FUNDAMENTAL and every service is FUNDAMENTAL or PERFECT. No address required.
- Deadline is 300 s by default (`SCHEMA_PI_DEADLINE`). After two failed boots: known-good if it differs, else systemd for one boot. The counter resets to 0 on systemd fallback.
- amd64 ships packages only. `schema-pi enable` refuses on any machine without `/boot/firmware/cmdline.txt`.
- Repo URL: `https://ajax80.github.io/schema-init-apt`. Suites `stable` and `dev`, component `main`. Dev keeps the last 10 versions per arch.
- Versions: stable `<spec Version>-1`. Dev `<spec Version>-1.<rev-list count>.<YYYYMMDD>git<sha7>`, using the same counter as `Makefile:227`.
- Private signing key lives only in an Actions secret and in KeePassXC, never in git.
- Fedora behaviour is unchanged. No `init.c`/`engine.c` logic change, apart from the opt-in `SCHEMA_SVC_DIR` read in Task 2.

## Review Focus

1. **Power cut mid-write of the boot counter.** A torn or empty `pi-boot` file must read as `attempts=0` and boot current, never crash the wrapper. Test in Task 1 (`decide_garbage_state`).
2. **Root mounted read-only** when the wrapper starts as PID 1. It must remount rw before writing. If the remount fails it must still exec the current engine, never panic. Covered by the Task 7 vmtest, which boots with `ro`.
3. **`cmdline.txt` edited twice or by hand** (an existing `init=`, trailing newline, CRLF). enable and disable must be idempotent and keep exactly one line. Tests in Task 4.
4. **A service that is legitimately oneshot-failed by design**, such as an optional hardware probe. It blocks boot-ok, which is the decided semantics, so the bootok log must name the service. Test in Task 3 (`bootok_names_blocker`).
5. **`apt upgrade` while PID 1 is systemd** (the fallback boot, or a not-enabled box). The trigger must not run `schema-ctl reexec`. Test in Task 5.

---

### Task 1: Boot decision logic (pure)

**Files:**
- Create: `pi_bootguard.h`
- Test: `tests/test_pi_bootguard.c`
- Modify: `Makefile` (add the test to `test:`)

**Interfaces:**
- Produces:
  - `enum pi_target { PI_CURRENT, PI_KNOWN_GOOD, PI_SYSTEMD };`
  - `struct pi_state { int attempts; enum pi_target last_target; };`
  - `int pi_state_parse(const char *buf, struct pi_state *st);` Parses `attempts=N\nlast_target=current|known-good|systemd\n`. Anything unparsable gives `{0, PI_CURRENT}`. Returns 0.
  - `int pi_state_format(const struct pi_state *st, char *buf, size_t n);`
  - `enum pi_target pi_decide(struct pi_state *st, int known_good_differs);` Mutates `st`. Rules: attempts 0 or 1 → CURRENT, attempts+1. attempts == 2 and differs → KNOWN_GOOD, attempts = 3. Otherwise → SYSTEMD, attempts = 0. Sets `last_target`.

- [ ] **Step 1: Write the failing tests** in `tests/test_pi_bootguard.c`, using the repo's plain-assert C test style.
  - `decide_fresh`: `{0}` gives CURRENT, attempts 1. Again gives CURRENT, attempts 2.
  - `decide_third_with_kg`: `{2}` with differs=1 gives KNOWN_GOOD, attempts 3.
  - `decide_third_no_kg`: `{2}` with differs=0 gives SYSTEMD, attempts 0.
  - `decide_kg_failed`: `{3}` gives SYSTEMD, attempts 0.
  - `decide_after_systemd`: the state left by SYSTEMD then decided gives CURRENT. This proves systemd is one boot only.
  - `decide_garbage_state`: parse `""`, `"attempts="`, `"\x00\xff"` and `"attempts=99999999999"`. Each gives `{0, PI_CURRENT}`.
  - `roundtrip`: format then parse gives an equal struct.
- [ ] **Step 2:** `make test` fails because `pi_bootguard.h` doesn't exist.
- [ ] **Step 3:** Implement `pi_bootguard.h` as `static inline` functions, with no libc beyond `string.h`/`stdlib.h`.
- [ ] **Step 4:** `make test` passes.
- [ ] **Step 5:** Commit `pi_bootguard.h tests/test_pi_bootguard.c Makefile`.

### Task 2: The `schema-init-pi` wrapper and the `SCHEMA_SVC_DIR` opt-in

**Files:**
- Create: `schema-init-pi.c`
- Modify: `svc_dropins.h` (helper), `init.c` (one call site where `engine_paths.svc_dir` is used as PID 1), `Makefile` (static target `schema-init-pi`, added to `BINS`)
- Test: `tests/test_pi_wrapper.sh`, `tests/test_svc_dir_env.c`

**Interfaces:**
- Consumes: Task 1, all of it.
- Produces:
  - Binary `/usr/sbin/schema-init-pi` (static).
  - State file `/var/lib/schema-init/pi-boot` in Task 1's format.
  - Fallback log `/var/log/schema-init/fallback.log`. One line per fallback: `<epoch> fallback=<known-good|systemd> attempts=<n>`.
  - Known-good layout `/var/lib/schema-init/known-good/{schema-init,schema-ctl,services/}`.
  - Env `SCHEMA_SVC_DIR=/var/lib/schema-init/known-good/services` is set only when exec'ing known-good.
  - `static inline const char *svc_dir_from_env(const char *builtin)` in `svc_dropins.h`. It returns `getenv("SCHEMA_SVC_DIR")` when that is an absolute path to an existing directory, else `builtin`.
- Test-only env, honoured only when `getpid() != 1`: `SCHEMA_PI_ROOT` (prefix for every path above plus `/usr/bin/schema-init` and `/lib/systemd/systemd`).

Wrapper behaviour, in order:
1. If PID 1, `mount("", "/", NULL, MS_REMOUNT, NULL)`. On failure, log to `/dev/kmsg` and exec current with no state write.
2. Read the state.
3. Set `known_good_differs` to 1 when `known-good/schema-init` exists and its bytes differ from `/usr/bin/schema-init`.
4. Call `pi_decide`.
5. Write the state with `write` + `fsync` + `rename` + directory `fsync`.
6. Append to `fallback.log` (fsync) if the target is not CURRENT.
7. Exec:
   - **CURRENT:** stdout/stderr go to `/var/log/schema-init/boot.log` (O_TRUNC). Exec `/usr/bin/schema-init` with argv passed through.
   - **KNOWN_GOOD:** same, but exec `known-good/schema-init` with `SCHEMA_SVC_DIR` set.
   - **SYSTEMD:** open `/dev/console` onto fds 0, 1 and 2, then exec `/lib/systemd/systemd` with `argv[0]` set to that path.
   - If any exec fails, try the next target down. If all fail, `pause()` forever (never exit as PID 1).

- [ ] **Step 1: Write the failing tests.**
  - `tests/test_svc_dir_env.c`:
    - unset gives builtin
    - a relative path gives builtin
    - a missing dir gives builtin
    - an existing absolute dir gives that dir
  - `tests/test_pi_wrapper.sh`: builds a fake root in a tmpdir with stub `usr/bin/schema-init`, `known-good/schema-init` and `lib/systemd/systemd`. Each stub prints its name, `$SCHEMA_SVC_DIR` and its args. Run the wrapper four times with `SCHEMA_PI_ROOT`. Assert:
    - output sequence `schema-init, schema-init, known-good, systemd, schema-init`
    - args `a b` passed through every time
    - `pi-boot` shows `attempts=0` after the systemd run
    - `fallback.log` has 2 lines
    - known-good identical to current gives `schema-init, schema-init, systemd`
- [ ] **Step 2:** Run both tests. Both fail.
- [ ] **Step 3:** Implement `svc_dir_from_env`, and use it in `init.c` only where `getpid() == 1` picks the built-in dir. The existing guard comment at `init.c:1658` explains why positional args are ignored, and env is not cmdline. Implement `schema-init-pi.c`. Makefile: `schema-init-pi: schema-init-pi.c pi_bootguard.h` with `$(CC) $(CFLAGS) -static -o $@ $<`.
- [ ] **Step 4:** Both tests pass, and `make test` is green, including `check_tu_statics.sh`.
- [ ] **Step 5:** Commit.

### Task 3: `pi-bootok` and `pi-deadline` services

**Files:**
- Create: `scripts/schema-pi-bootok`, `scripts/schema-pi-deadline`, `services/pi-bootok.svc.example`, `services/pi-deadline.svc.example`
- Test: `tests/test_pi_bootok.sh`

**Interfaces:**
- Consumes:
  - `schema-ctl status --kv` lines `service.<name>.state=<STATE>`
  - Task 2's state file and known-good layout
- Produces:
  - Marker `/run/schema-init/pi-boot-ok`, meaning this boot was confirmed.
  - Installed paths `/usr/libexec/schema-init/schema-pi-bootok` and `/usr/libexec/schema-init/schema-pi-deadline`.

Details:
- **bootok** polls every 5 s until `sshd` is FUNDAMENTAL and every `service.*.state` (except `pi-bootok` and `pi-deadline` themselves) is FUNDAMENTAL or PERFECT. Then it:
  - writes `attempts=0`, keeping `last_target`
  - touches the marker
  - if `last_target=current`, promotes:
    - copies `/usr/bin/schema-init`, `/usr/bin/schema-ctl` and `/etc/schema-init/services/` into `known-good.new/`, then renames it over `known-good/`
    - copies the two `/usr/libexec/schema-init/schema-pi-*` scripts too
  - logs `pi-bootok: boot confirmed after <s>s (target=<t>)`
  
  While waiting, every 60 s it logs `pi-bootok: waiting on <name>=<STATE>` for each blocker. bootok is the oneshot owner, so the deadline is not its job.
- **deadline** waits `${SCHEMA_PI_DEADLINE:-300}` seconds. If the marker is missing, it logs `pi-deadline: no boot-ok after <n>s, rebooting` and runs `schema-ctl reboot`.
- Test overrides: `SCHEMA_PI_ROOT`, `SCHEMA_CTL` (path of a fake ctl) and `SCHEMA_PI_POLL` (poll interval).

- [ ] **Step 1: Write the failing tests** in `tests/test_pi_bootok.sh`, using a fake `schema-ctl` that prints canned kv:
  - `bootok_confirms`: all services FUNDAMENTAL/PERFECT. Marker appears, `attempts=0`, known-good populated with services.
  - `bootok_names_blocker`: `wpa-supplicant=RECOVERY`. No marker after 2 polls, and the log contains `waiting on wpa-supplicant=RECOVERY`.
  - `bootok_no_promote_on_kg`: `last_target=known-good`. Marker appears and `known-good/` is unchanged (compare mtimes).
  - `deadline_reboots`: deadline 1 s, no marker. The fake ctl records `reboot`.
  - `deadline_quiet`: marker present. No reboot recorded.
- [ ] **Step 2:** Run. Fails.
- [ ] **Step 3:** Implement both scripts in POSIX sh. The `.svc` examples:
  - `pi-bootok`: `oneshot=1`, `needs_root=1`, `critical=0`
  - `pi-deadline`: `oneshot=0`, `needs_root=1`, `critical=0`
  
  The service format uses no `dep=` for the deadline, since it starts at boot.
- [ ] **Step 4:** Passes.
- [ ] **Step 5:** Commit.

### Task 4: `schema-pi enable | disable | status`

**Files:**
- Create: `scripts/schema-pi` (installed `/usr/sbin/schema-pi`)
- Test: `tests/test_schema_pi.sh`

**Interfaces:**
- Consumes: Task 2's paths, Task 3's `.svc` examples (installed by enable)
- Produces:
  - Backup `/boot/firmware/cmdline.txt.schema-bak`
  - Kernel argument `init=/usr/sbin/schema-init-pi`

Details:
- **enable** refuses, with a message and exit 1, if:
  - `/boot/firmware/cmdline.txt` is missing
  - `/lib/systemd/systemd` is not executable
  - `/var/lib` is on a `noexec` mount (`findmnt -no OPTIONS -T /var/lib`)
  - `sshd.svc` is not in `/etc/schema-init/services/`
  
  Otherwise it:
  - copies the backup once
  - removes any `init=` token, then appends ` init=/usr/sbin/schema-init-pi`
  - keeps one line, strips CR, keeps the trailing newline
  - installs the two `.svc` files if absent
  - seeds known-good (same copy as bootok's promote)
  - writes `attempts=0`
- **disable** removes every `init=` token (systemd is the Pi OS default) and leaves the rest of the line byte-identical.
- **status** prints:
  - enabled yes/no
  - PID 1 (from `/proc/1/comm`)
  - `attempts` and `last_target`
  - the last `fallback.log` line
  - the known-good engine version (`known-good/schema-init --version`)
- Test override: `SCHEMA_PI_ROOT`.

- [ ] **Step 1: Write the failing tests:**
  - `enable_fresh`: a stock Pi OS line gives the same line plus ` init=/usr/sbin/schema-init-pi`, with one `\n`.
  - `enable_twice`: identical bytes after the second run.
  - `enable_replaces_old_init`: a line with `init=/sbin/schema-init-log` ends up with exactly one `init=` token, the new one.
  - `enable_crlf`: CRLF input gives LF output, still one line.
  - `enable_refuses_no_systemd`, `enable_refuses_noexec` (fake `findmnt` on PATH), `enable_refuses_no_cmdline` (the amd64 case): each gives exit 1 and leaves the file untouched.
  - `disable_roundtrip`: enable then disable gives the original bytes.
- [ ] **Step 2:** Run. Fails.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Passes.
- [ ] **Step 5:** Commit.

### Task 5: Debian packaging

**Files:**
- Create:
  - `debian/control`, `debian/rules`, `debian/source/format` (`3.0 (native)`)
  - `debian/schema-init.install`, `debian/schema-init.postinst`, `debian/schema-init.triggers`
  - `debian/copyright`
  - `tools/deb-changelog.sh`
- Test: `tests/test_deb_trigger.sh`, `tests/test_deb_version.sh`

**Interfaces:**
- Consumes: binaries from Tasks 2–4, plus the existing `schema-init schema-ctl schema-subreaper schema-journal-sink schema-recorder schema-coredump schema-board schema-user`
- Produces:
  - `.deb` `schema-init_<ver>_<arch>.deb`
  - `tools/deb-changelog.sh <stable|dev>` writes `debian/changelog` and prints the version

Details:
- **control:**
  - `Build-Depends: debhelper-compat (= 13), libzstd-dev, libacl1-dev, pkgconf`
  - `Depends: systemd, python3`
  - no `-daemons`
  - `Architecture: any`
- **rules:** builds `make BINS="schema-init schema-user schema-ctl schema-subreaper schema-journal-sink schema-board schema-coredump schema-recorder" schema-init-pi`. Install layout:
  - binaries in `/usr/bin`
  - `schema-init-pi` and `schema-pi` in `/usr/sbin`
  - `schema-pi-*` scripts in `/usr/libexec/schema-init`
  - `.svc.example` files in `/usr/share/schema-init/`
  
  `schema-analyze` is attempted only if `cargo` is present, and its failure does not fail the build (spec: my call). After `dh_install`, the rules fail if `find debian/schema-init/usr/local` finds anything.
- **triggers:** `interest-noawait schema-init-reexec`
- **postinst:**
  - On `configure`: `dpkg-trigger --no-await schema-init-reexec`. Print the enable hint on first install only (`[ -z "$2" ]`).
  - On `triggered`: if `[ "$(cat /proc/1/comm)" = schema-init ]` and `[ /proc/1/root -ef / ]`, run `schema-ctl reexec` or print the same fallback message as `schema-init.spec:472`.
- **Version** reads `Version:` from `schema-init.spec`. Dev appends `.$(git rev-list --count HEAD).$(date +%Y%m%d)git$(git rev-parse --short=7 HEAD)`.

- [ ] **Step 1: Write the failing tests:**
  - `test_deb_version.sh`: on a tagged checkout, stable gives `0.4.20-1` (read from the spec). Dev matches `^0\.4\.20-1\.[0-9]+\.[0-9]{8}git[0-9a-f]{7}$`. `dpkg --compare-versions` shows dev `gt` stable and next stable `gt` dev.
  - `test_deb_trigger.sh`: run `debian/schema-init.postinst triggered schema-init-reexec` with a fake `/proc/1/comm` via `SCHEMA_PI_ROOT`-style override `SCHEMA_PROC1`, and a fake `schema-ctl` on PATH:
    - comm=`systemd`: no reexec call
    - comm=`schema-init`: one reexec call
    - `configure` calls `dpkg-trigger` and not `schema-ctl`
- [ ] **Step 2:** Run. Fails.
- [ ] **Step 3:** Implement the packaging files.
- [ ] **Step 4:** Both tests pass. Then on blakbox:
  ```
  podman run --rm -v $PWD:/src:Z debian:trixie sh -c 'apt-get update && apt-get -y build-dep /src && cd /src && tools/deb-changelog.sh dev && dpkg-buildpackage -b -us -uc && lintian ../schema-init_*.deb'
  ```
  It must build, show no lintian **errors**, and `dpkg -c` must show no `/usr/local`.
- [ ] **Step 5:** Commit (not `debian/changelog`, which is generated; add it to `.gitignore`).

### Task 6: CI build, ARMv6 gate, publish

**Files:**
- Create: `.github/workflows/deb.yml`, `tools/raspbian-rootfs.sh`, `tools/check-armv6.sh`
- Create, in the new repo `ajax80/schema-init-apt`: `.github/workflows/publish.yml`, `tools/build-repo.sh`, `README.md`, `schema-init-archive-keyring.gpg`

**Interfaces:**
- Consumes: Task 5 (`tools/deb-changelog.sh`, `dpkg-buildpackage`)
- Produces:
  - Artifact `debs-<channel>` containing every `.deb`
  - `repository_dispatch` event `pi-debs` with payload `{run_id, channel}`
  - Repo layout `dists/{stable,dev}/main/binary-{armhf,arm64,amd64}/Packages{,.gz}`, `dists/*/InRelease`, `dists/*/Release.gpg`, `pool/main/s/schema-init/*.deb`

Details:
- **deb.yml** runs on push to `master` (channel dev) and on tags `v*` (channel stable). Jobs:
  - **amd64:** `ubuntu-latest` with a `debian:trixie` container.
  - **arm64:** `ubuntu-24.04-arm` with a `debian:trixie` container.
  - **armhf:** `ubuntu-latest`. Install `qemu-user-static binfmt-support debootstrap`. Then `tools/raspbian-rootfs.sh`, which runs `debootstrap --arch=armhf --keyring=<raspbian.public.key> trixie rootfs http://raspbian.raspberrypi.com/raspbian`, cached by `actions/cache` keyed on the week. Build by chroot into it.
  
  Each job runs `make test` (armhf under QEMU; skip the same live-only tests `ci-python-suite.sh` skips) and builds the deb. armhf then runs `tools/check-armv6.sh`. **publish** needs all three: it uploads the artifact and sends the dispatch with secret `APT_DISPATCH_TOKEN`.
- **check-armv6.sh** unpacks every `.deb` and runs `readelf -A` on every ELF. It fails unless every ELF's `Tag_CPU_arch` is exactly `v6` or `v6KZ`.
- **publish.yml** (schema-init-apt) runs on `repository_dispatch: pi-debs` and on `workflow_dispatch`. Steps:
  1. `gh run download <run_id> -R ajax80/schema-init`
  2. Fetch the current pool from the live site by reading each live `Packages` and downloading the listed debs. An empty site is fine on the first run.
  3. Add the new debs to the channel.
  4. Prune dev to the newest 10 versions per arch with `dpkg --compare-versions`.
  5. `apt-ftparchive packages/release` per suite.
  6. Sign with `APT_SIGNING_KEY` (`gpg --clearsign` gives `InRelease`, `--detach-sign` gives `Release.gpg`).
  7. `actions/upload-pages-artifact`, then `actions/deploy-pages`.
- **Human steps.** These are outward-facing; ask Jonathan before each:
  - create the `ajax80/schema-init-apt` repo and enable Pages from Actions
  - generate the repo key (ed25519, no expiry decision is his) and store it in KeePassXC
  - add the `APT_SIGNING_KEY` secret
  - create a fine-grained PAT scoped to schema-init-apt `contents:write` and store it as `APT_DISPATCH_TOKEN`

- [ ] **Step 1: Write the failing test.** `tools/check-armv6.sh` run on the amd64 deb from Task 5 must exit 1 (no ARM tags). On a deb holding a v7 binary (cross-built with `arm-linux-gnueabihf-gcc -march=armv7-a` in a debian container), it must exit 1.
- [ ] **Step 2:** Run. It fails before the script exists.
- [ ] **Step 3:** Implement the scripts and workflows. Push on a branch. In schema-init-apt, run `workflow_dispatch` against that branch's run_id.
- [ ] **Step 4:** All three build jobs are green, the armhf gate passes, and publish deploys. Then from a `debian:trixie` container on each arch (amd64 locally; arm64 and armhf under QEMU on blakbox):
  - add the deb822 source from the README
  - `apt-get update` shows no signature warnings
  - `apt-get install -y schema-init`
  - `schema-init --version` prints the dev version
- [ ] **Step 5:** Commit both repos.

### Task 7: Fallback proven in the VM harness

**Files:**
- Modify: `tests/livetest/vmtest.sh` (new mode `PI_WRAPPER=1`)

**Interfaces:**
- Consumes: Task 2 (`schema-init-pi`, state format, `SCHEMA_SVC_DIR`), Task 3 (bootok).
- Produces: vmtest checks `PI-current`, `PI-knowngood`, `PI-systemd` and `PI-ro`.

Details: with `PI_WRAPPER=1` the harness:
- packs `schema-init-pi` as `/init`
- puts the real engine at `/usr/bin/schema-init` and a stub `/lib/systemd/systemd` that prints `PI-SYSTEMD-REACHED` to the console
- seeds `/var/lib/schema-init/pi-boot` per boot
- boots 4 times reusing one disk image: attempts 0, 2 with differing known-good, 3, and 0 with `ro` on the cmdline

- [ ] **Step 1:** Add the checks. Expected serial markers:
  - boot 1: rail runs and pi-bootok logs `boot confirmed`
  - boot 2: the known-good engine runs with svc dir `/var/lib/schema-init/known-good/services`
  - boot 3: `PI-SYSTEMD-REACHED` appears on the console, proving the console was reopened
  - boot 4: the rail runs despite `ro`
- [ ] **Step 2:** Run against master's binaries with the mode stubbed. It must MISS.
- [ ] **Step 3:** Implement the mode.
- [ ] **Step 4:** `PI_WRAPPER=1 OUT=$HOME/schema-livetest tests/livetest/vmtest.sh` gives `>> RESULT: PASS`. The normal `tests/livetest/vmtest.sh` still passes.
- [ ] **Step 5:** Commit. Open the PR as **draft**, run one `/code-review`, fold in the findings, then mark it ready.

### Task 8: Docs and hardware rollout

**Files:**
- Create: `distros/raspberry-pi/README.md` (supersedes the install part of `distros/raspberry-pi-zero-w/README.md`, which keeps its gotchas and links to the new file)
- Modify: `README.md` (one install pointer)

**Interfaces:**
- Consumes: the repo URL, `schema-pi`, the fallback behaviour.

The README covers:
- install: keyring, deb822 `.sources` with `Signed-By:`, `apt install schema-init`
- `sudo schema-pi enable`
- what boot OK means and how the fallback behaves (two failed boots, then known-good, then systemd for one boot, with no network address required)
- `schema-pi status`
- `schema-pi disable`
- recovery by hand: remove `init=…` from `cmdline.txt` on the SD card from another computer
- amd64: packages only
- fix the "Pi OS Trixie (Debian Bookworm)" line in the old README

- [ ] **Step 1:** Write the docs.
- [ ] **Step 2: OrangePi (armhf, dev).** Ask Jonathan first. Back up `/sbin/schema-init*`, `/etc/schema-init`, `/usr/local/bin/schema-*` and `cmdline.txt` to `/root/pre-deb-20261028/`. Then:
  - add the dev source
  - `apt install schema-init`
  - `schema-pi enable`, which replaces `init=/sbin/schema-init-log`
  - reboot
  
  Pass when:
  - PID 1 is `schema-init`
  - all 15+2 services are FUNDAMENTAL/PERFECT
  - `pi-boot-ok` marker present
  - tailscale serve returns 200
  - `known-good/` is populated
- [ ] **Step 3: OrangePi fallback.** Copy a binary that exits immediately over `/usr/bin/schema-init` (a dpkg-owned file, restored by `apt install --reinstall` afterwards). Reboot. Expect two failed boots, then a known-good boot that is reachable. Restore. Reboot. Expect current.
- [ ] **Step 4: BlakPi (arm64, dev).** Ask first. Back up the same items. Write `sshd.svc` and a `pihole-FTL.svc` (from the kit and the OrangePi pattern). Install, enable, reboot. Pass when:
  - PID 1 is `schema-init`
  - `dig @192.168.8.223 example.com` answers
  - marker present
  
  Check `/dev/watchdog` exists and `bcm2835_wdt` is loaded on both Pis.

  Then one dev upgrade (merge any trivial commit): `apt upgrade` re-execs PID 1 live and Pi-hole stays up. Then a systemd fallback test: no known-good (`rm -r known-good`) plus the bad binary. Expect two failed boots, a systemd boot that is reachable over ssh, and the next boot back on schema.
- [ ] **Step 5:** Record results in memory (`project_schema_pi_packaging`). Tag the release only after the spec bump.
