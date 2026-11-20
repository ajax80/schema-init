# schema-ctl verify Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A read-only pre-reboot check (`schema-ctl verify`) that compares the next boot against the last proven-good boot, checks the boot entries and the ways back, and blocks live re-exec, `schema-snapshot arm` and terminal-attached reboot/poweroff on a real problem.

**Architecture:** The candidate `schema-init` binary gets a non-PID-1 `--check` mode that loads a target root's services with PID 1's own loader and prints a canonical, versioned description of every service. A good boot saves that output as the baseline. `schema-ctl verify` resolves which entry GRUB boots next, reads that entry's root (read-only, in a private mount namespace), runs the candidate's `--check`, diffs it against the baseline, and adds boot-entry, fallback and disk checks. Gates call it.

**Tech Stack:** C (gnu99, the project's CFLAGS, static PID 1 build), POSIX sh for scripts, the existing `make test` harness.

**Spec:** `docs/superpowers/specs/2026-11-19-schema-ctl-verify-design.md` — read it; this plan argues from it.

**Delivery:** three PRs, each its own branch off master, opened as **draft**, one `/code-review` pass, fixes, CI green, then merge (Jonathan's workflow). PR 1 = Tasks 1–4, PR 2 = Tasks 5–10, PR 3 = Tasks 11–13. Nothing blocks anything until PR 3.

## Global Constraints

- Never report PASS for an input that wasn't resolved: emit `[UNRESOLVED] <what>`; it counts as FAIL for re-exec/arm and WARN-and-proceed for interactive reboot/poweroff.
- `--check` runs only when `getpid() != 1`, never binds or connects the control socket, starts nothing (precedent: `--reexec-check`, `init.c:1747`).
- No second parser: everything about services comes from `services_load` / `engine_load` / `validate_and_resolve` on `service_t`.
- Exit codes of `schema-ctl verify`: `0` pass (WARN allowed), `1` any FAIL, `2` no FAIL but unaccepted CHANGE or lost baseline, `3` nothing worse than UNRESOLVED (re-exec/arm refuse on it; interactive reboot/poweroff WARN and proceed). Precedence 1 > 2 > 3 > 0.
- State dir `/var/lib/schema-init/verify/`, root-owned `0700`; `baseline`, `accepted` files `0600`; `established` marker. Every write: temp file + `rename`, `O_NOFOLLOW`. Env `SCHEMA_VERIFY_DIR` overrides the dir (tests only).
- Baseline format line 1: `schema-verify-baseline v1`.
- Boot-critical cmdline inputs compared: `root=`, `rootflags=`, `init=`, `rd.*`, `modprobe.*`, `SCHEMA_SVC_DIR=`, `schema.hardening_default=`.
- Settle window: 60 s on `CLOCK_MONOTONIC`; healthy = FUNDAMENTAL, SETTLED, FULL_TRUST, PERFECT; a restart or an unhealthy state restarts the window.
- Gates apply only when stdin and stderr are both ttys; `--force` always overrides; system callers that must never block pass `--force` explicitly.
- No new runtime dependencies. `schema-init` static build and `check_tu_statics.sh` must keep passing.
- Repo style: no docstrings, why-comments only, `printf` over heredoc in shell.

## Review Focus

1. **A `.svc` key added later that the serializer doesn't know** → must fail a test, not silently escape the baseline. (Task 1, `test_svc_serial_keys.sh`.)
2. **Running unprivileged** (CI, a user typing `schema-ctl verify`) → sections needing root report `[UNRESOLVED] needs root` / skipped, never PASS and never crash. (Task 9, CLI test as non-root.)
3. **btrfs layout where GRUB's root is the top level** (`linux /root/boot/vmlinuz-…`, `blsdir=/root/boot/loader/entries`) vs a separate `/boot` partition → both map to the right files. (Task 6, path-mapping tests for both layouts.)
4. **A service whose `.svc` is present but refused by owner/mode** (`svc_refused`) → FAIL naming the file, not "missing". (Task 2, fixture with a group-writable file.)
5. **Verifier hangs** (candidate binary wedges, NFS stat blocks) → gates time out after 30 s and treat it as "verifier unavailable". (Task 11, stub that sleeps.)

---

## PR 1 — `--check`, serializer, baseline writers

### Task 1: Canonical service serializer

**Files:**
- Create: `svc_serial.c`, `svc_serial.h`, `sha256.c`, `sha256.h`
- Modify: `Makefile` (add to `OBJS` used by schema-init/schema-user and to the test list), `engine.c` (status `<svc>` hardening block calls the serializer's formatter for the overlapping fields)
- Test: `tests/test_svc_serial.c`, `tests/test_svc_serial_keys.sh`

**Interfaces:**
- Produces:
  - `int svc_serialize(const service_t *s, FILE *out);` — writes one line per property: `<name>.<key>=<value>` then, when the property has a source, a tab and `src=explicit|default|dropped`. Values escape `\n`, `\t`, `\\`. Returns lines written.
  - `int svc_serialize_all(const service_t *tab, int n, FILE *out);` — all services, lines sorted bytewise.
  - `void sha256_hex(const void *data, size_t len, char out[65]);`
- Field coverage: the spec's serializer table (Execution … Resources). Seccomp set as `sc.hash=<sha256 of sorted syscall numbers>` + `sc.count=`.

- [ ] **Step 1: Write failing tests** in `tests/test_svc_serial.c` — load fixture `.svc` text via `service_load_one` into two `service_t`s and assert:
  - `test_args_change`: `exec=/usr/bin/mount` + `args=/home` vs `args=/tmp` → serialized outputs differ in a line starting `x.argv=`.
  - `test_exec_pre_change`: differing `exec_pre=` only → outputs differ.
  - `test_source_only`: `no_new_privs=1` explicit vs defaulted (hardening default on) → the `x.no_new_privs=1` value part identical, `src=` differs.
  - `test_sorted_stable`: `svc_serialize_all` twice → byte-identical; lines sorted.
  - `test_sha256`: `sha256_hex("abc")` == `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
- [ ] **Step 2: Write `tests/test_svc_serial_keys.sh`**: extract every key the parser accepts from `service.c` (`strcmp(line, "…")` / `strcmp(key, "…")` in the parse function) and assert each appears in `svc_serial.c`'s key table, minus an explicit allowlist (`name`, `description` and any purely cosmetic key — each allowlisted key gets a why-comment).
- [ ] **Step 3: Run both, verify they fail** (`make test` target lines added; expected: link error / missing keys).
- [ ] **Step 4: Implement** `svc_serial.c` (a static table of `{key, emitter}`), `sha256.c` (plain FIPS 180-4, no deps). Switch `engine.c`'s status hardening lines to the shared formatter where fields overlap, output text unchanged (`schema-ctl status avahi` must print exactly what it prints today — check `tests/` status assertions still pass).
- [ ] **Step 5: Run `make test`**; expected: new tests pass, existing pass.
- [ ] **Step 6: Commit** `feat(serial): canonical per-service serializer + sha256`.

### Task 2: `schema-init --check`

**Files:**
- Create: `check.c`, `check.h`
- Modify: `init.c` (argv: `--check`, `--root`, `--cmdline`, `--svc-dir`; dispatch before the not-PID-1 refusal), `schema-init-pi.c` (extract target choice), `Makefile`
- Test: `tests/test_check_mode.sh`, fixtures under `tests/fixtures/check/`

**Interfaces:**
- Consumes: `svc_serialize_all` (Task 1), `engine_load`, `services`/`svc_count` (`engine.h`), `hardening_default_resolve` + `service_set_hardening_default` (`service.h`).
- Produces:
  - CLI: `schema-init --check --root <dir> --cmdline "<opts>" [--svc-dir <dir>]`; exit 0 all loaded, 1 otherwise.
  - Output (stdout), in order:
    ```
    schema-verify-baseline v1
    binary_version=<SCHEMA_INIT_VERSION>
    cmdline.root=… / cmdline.rootflags=… / cmdline.init=… / cmdline.rd.*=… / cmdline.modprobe.*=… / cmdline.SCHEMA_SVC_DIR=… / cmdline.schema.hardening_default=…
    hardening_default=on|off
    svc_dir=<path relative to root>
    error: <file>: <reason>      (one per refused/unparsed .svc, cycle, load failure)
    note: <text>                 (dropped defaults, other loader stderr)
    <svc_serialize_all lines>
    ```
    (`created=` is added by the baseline writer, Task 3, not here.)
  - `const char *check_svc_dir(const char *root, const char *cmdline, char *buf, size_t n);` — `SCHEMA_SVC_DIR=` from cmdline (validated as PID 1 validates it) else `/etc/schema-init/services`, prefixed with root.
  - `schema-init-pi.c`: `enum pi_target pi_choose_target(const struct pi_state *st);` exported via a header so `check_svc_dir` applies the known-good rule when the Pi wrapper would pick `PI_KNOWN_GOOD`.

- [ ] **Step 1: Write `tests/test_check_mode.sh`** (runs as the CI user, builds `./schema-init`; fixtures owned by the user so `service_open_trusted` accepts them). Assertions:
  - clean dir → exit 0, line 1 `schema-verify-baseline v1`, one `x.exec=` line per service.
  - parse error fixture → exit 1, `error: bad.svc:` line.
  - cycle (a→b→a) → exit 1, `error:` mentioning cycle.
  - group-writable `.svc` → exit 1, `error: gw.svc:` (Review Focus 4).
  - `dep=nosuch` → exit 0 (PID 1 treats it as satisfied); `dep=display-stack` with `display-stack.grp` present → no note.
  - default hiding exec (`exec=/home/u/bin/x`, hardening default on) → `note:` with "dropped".
  - `--cmdline "schema.hardening_default=0"` vs `=1` → `hardening_default=` flips and `x.no_new_privs` changes.
  - `--root R` where `R/etc/schema-init/hardening-default` says `on` while the live `/etc` doesn't exist/differs → uses R's (`hardening_default=on`).
  - `--cmdline "SCHEMA_SVC_DIR=/alt"` → `svc_dir=/alt`, loads `R/alt`.
  - run as PID 1 is impossible in the test; instead assert `--check` never creates `$XDG_RUNTIME_DIR`/socket paths (strace-free: check no new socket file under the test's tmp root).
- [ ] **Step 2: Run, verify fail** (`unknown option '--check'`).
- [ ] **Step 3: Implement** `int check_main(const char *root, const char *cmdline, const char *svc_dir_override);` in `check.c`. Capture loader stderr by `dup2` onto a `tmpfile()` during `engine_load`, then classify: any `.svc` in the dir whose name didn't load → `error:` (authoritative, independent of message text); remaining captured lines → `note:`. Groups load through `engine_load` (it already calls `groups_load`).
- [ ] **Step 4: Run `make test`**, expect pass; `make schema-init` static build still links (check `ldd schema-init` = not dynamic).
- [ ] **Step 5: Commit** `feat(init): --check mode reports a target root's services`.

### Task 3: Baseline writer and settle wait in schema-ctl

**Files:**
- Create: `verify_baseline.c`, `verify_baseline.h`
- Modify: `schema-ctl.c` (`verify --write-baseline [--wait-settled <secs>]`; factor the inline socket request into `static int ctl_request(const char *req, char **reply)` reused by `status`), `Makefile` (schema-ctl links `verify_baseline.o sha256.o`)
- Test: `tests/test_verify_baseline.c`

**Interfaces:**
- Consumes: `schema-init --check` output format (Task 2).
- Produces:
  - `int baseline_write(const char *dir, const char *check_out, size_t len, time_t now);` — prepends nothing, inserts `created=<epoch>` after line 2, writes `dir/baseline` atomically, creates `dir/established`; refuses (returns -1, errno `ELOOP`) when `dir/baseline` or `dir` is a symlink; refuses when `dir` is on tmpfs (`statfs` `TMPFS_MAGIC`, errno `EROFS`-style message "var not persistent").
  - `typedef struct { uint64_t since_ms; char sig[65]; } settle_t;` and `int settle_step(settle_t *st, const char *status_kv, uint64_t now_ms, unsigned window_s);` → 1 when settled. `sig` = hash of the (name, restarts) set; any change or any non-healthy `state=` resets `since_ms`.
  - `schema-ctl verify --write-baseline`: runs `/proc/1/exe --check --root / --cmdline "$(cat /proc/cmdline)"`, then `baseline_write`. Exit 0 on success.
- [ ] **Step 1: Failing tests** in `tests/test_verify_baseline.c`:
  - `test_atomic_perms`: write into a tmp dir → `baseline` mode `0600`, `established` exists, `created=` is line 3.
  - `test_symlink_refused`: `baseline` pre-created as a symlink → -1, target untouched.
  - `test_settle`: feed kv with all FUNDAMENTAL at t=0, t=30 s, t=61 s → 0,0,1; a restart count bump at t=50 s → not settled until t=111 s; one `state=RECOVERY` at t=40 s → resets.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement.** `--wait-settled` polls `status --kv` once per second via `ctl_request`, gives up after 15 minutes with exit 3 and a log line.
- [ ] **Step 4: `make test` passes; `schema-ctl status` output unchanged** (existing tests).
- [ ] **Step 5: Commit** `feat(ctl): verify --write-baseline and settle wait`.

### Task 4: Wire the writers; packaging

**Files:**
- Modify: `scripts/schema-bootok` (after `grub2-editenv … boot_success=1`: `schema-ctl verify --write-baseline || echo "schema-bootok: baseline not written" >&2`), `scripts/schema-pi-bootok` (same at its success point), `schema-init.spec`, `debian/schema-init.install`
- Create: `services/schema-verify-baseline.svc` (oneshot, `exec=/usr/bin/sh`, args run: exit 0 if `schema-ctl status schema-bootok` succeeds, else `schema-ctl verify --write-baseline --wait-settled 60`; `critical=0`; `syscall_deny=0` explicit like other shipped `.svc`)
- Test: extend `tests/test_bootok.sh`, `tests/test_pi_bootok.sh`

- [ ] **Step 1: Failing test additions**: with a stub `schema-ctl` on `PATH` that records argv, a successful bootok run records `verify --write-baseline`; a timed-out run records nothing.
- [ ] **Step 2: Run, verify fail.**
- [ ] **Step 3: Implement**; package the `.svc` in rpm `%files` and the deb install list (installed as an example on the Pi kit if that's the existing pattern for `pi-*.svc.example`).
- [ ] **Step 4: `make test` passes.**
- [ ] **Step 5: Commit** `feat(verify): good boots write the baseline`. Open PR 1 as draft → `/code-review` → fix → CI green → merge.

---

## PR 2 — the `verify` report

### Task 5: Boot-entry model (pure parsing)

**Files:**
- Create: `verify_boot.c`, `verify_boot.h`
- Test: `tests/test_verify_boot.c`, fixtures `tests/fixtures/verify/boot/`

**Interfaces:**
- Produces:
  - `int grubenv_read(const char *path, char (*kv)[2][256], int max);`
  - `typedef struct { char id[256]; char linux_path[512]; char initrd[8][512]; int n_initrd; char options[4096]; int blank_before_last; int parse_err; } bls_entry_t;` and `int bls_parse(const char *path, bls_entry_t *e);`
  - `int karg_split(const char *opts, char (*out)[512], int max);` — kernel-argument rules (double quotes group, no backslash escapes).
  - `int karg_check(char (*args)[512], int n, char *why, size_t whysz);` — FAIL on duplicate/conflicting `root=`/`rootflags=`/`init=`, missing `root=`.
  - `typedef struct { const char *actual, *normal, *fallback; int unresolved; } nextboot_t;` and `int nextboot_resolve(char (*env)[2][256], int nenv, const bls_entry_t *ents, int nents, nextboot_t *nb);` — `next_entry` > `saved_entry` > first entry; then the `09_schema_fallback` rule (spec §3); a `schema_fallback_id` matching no entry sets `unresolved` and a FAIL reason.
  - `int grubcfg_live_fallback(const char *cfg_text);` — 1 only if `set default="${schema_fallback_id}"` appears uncommented inside the `boot_counter`/`boot_success` `if` and after the header's last `set default=`.
- [ ] **Step 1: Failing tests** — one per fixture: blank `initrd` line → `blank_before_last=1`; two `initrd` lines → `n_initrd=2`; missing `root=` → FAIL; duplicate `root=` → FAIL; quoted arg with space stays one token; `next_entry` set → actual = it; counter `0` + `boot_success=0` + fallback set → actual = fallback, normal = saved; fallback id unknown → unresolved; grub.cfg with the line only in a `#` comment → 0; real copy of blakbox's generated block → 1.
- [ ] **Step 2: Run, verify fail.** **Step 3: Implement.** **Step 4: Pass.** **Step 5: Commit** `feat(verify): boot entry and next-boot model`.

### Task 6: Target-root access

**Files:**
- Create: `verify_root.c`, `verify_root.h`
- Test: `tests/test_verify_root.c`

**Interfaces:**
- Consumes: `bls_entry_t`, `karg_split` (Task 5).
- Produces:
  - `int dev_resolve(const char *spec, const char *devdir, char *out, size_t n);` — `UUID=`/`PARTUUID=`/`LABEL=`/path via `<devdir>/disk/by-uuid|by-partuuid|by-label`; `devdir` defaults `/dev`, env `SCHEMA_VERIFY_DEVDIR` overrides (tests).
  - `int bls_path_map(const char *grub_root_mnt, const char *bls_path, char *out, size_t n);` — BLS paths are relative to GRUB's root: top-level-btrfs layout and separate-`/boot` layout (Review Focus 3).
  - `int root_inspect(const bls_entry_t *e, const char *check_args_extra, root_report_t *r);` — forks a child that `unshare(CLONE_NEWNS)`, makes `/` `MS_PRIVATE`, mounts the entry's `root=` device read-only (btrfs: `subvolid=5`) on a `mkdtemp` dir, checks: subvol from `rootflags=subvol=` exists, `linux` + every `initrd` exist (mapped), `init=` executable under the subvol, `<init> --version` runs, then runs `<subvol>/<init> --check --root <subvol> --cmdline "<options>"` and streams its stdout back over a pipe. Not root → `r->unresolved = "needs root"`. Child is killed after 30 s.
  - `typedef struct { int fail; char reasons[16][256]; int nreasons; const char *unresolved; char *check_out; size_t check_len; int check_rc; } root_report_t;`
- [ ] **Step 1: Failing tests**: `dev_resolve` against a fake devdir tree with symlinks; `bls_path_map` for both layouts (`/root/boot/vmlinuz-x` under top-level mount → `<mnt>/root/boot/vmlinuz-x`; `/vmlinuz-x` with a separate `/boot` → `/boot/vmlinuz-x`); `root_inspect` as non-root → `unresolved="needs root"`.
- [ ] **Step 2–4:** fail → implement → pass. The real mount path is exercised in Task 10's VM run.
- [ ] **Step 5: Commit** `feat(verify): inspect the next boot's root read-only`.

### Task 7: Baseline diff and accept

**Files:**
- Create: `verify_diff.c`, `verify_diff.h`
- Test: `tests/test_verify_diff.c`, fixtures `tests/fixtures/verify/diff/`

**Interfaces:**
- Consumes: Task 2 output format, Task 1 `sha256_hex`.
- Produces:
  - `typedef enum { F_FAIL, F_CHANGE, F_WARN, F_NOTE } fkind_t;` `typedef struct { fkind_t kind; char text[512]; } finding_t;`
  - `int diff_baseline(const char *base, const char *next, finding_t *out, int max, char changes_hash[65]);` — rules: `error:` in next → FAIL; service in base whose file is gone (no lines, no `error:`) → CHANGE "removed"; value change → CHANGE, grouped by key with service count and up to 6 names (`[CHANGE] syscall_deny  unset → default (default)   63 services: …`); source-only → NOTE; new service → WARN; boot-critical `cmdline.*` change → CHANGE; base format line ≠ `v1` → treated as lost baseline. Hash over sorted CHANGE texts only.
  - `int baseline_state(const char *dir, char **base_out);` → `BASE_OK`, `BASE_NEVER` (no `established`), `BASE_LOST` (established, baseline missing/unreadable/bad version).
  - `int accepted_matches(const char *dir, const char hash[65]);` and `int accept_write(const char *dir, const char hash[65]);` (atomic, same rules as Task 3).
- [ ] **Step 1: Failing tests**: the 11-18 fixture (63 services, base without `syscall_deny`, next with `x.syscall_deny=default\tsrc=default`) → exactly one grouped CHANGE naming `63 services`; argv-only change → CHANGE; source-only → NOTE, hash of empty set; removed `.svc` → CHANGE; refused `.svc` → FAIL; `BASE_NEVER` vs `BASE_LOST`; accept → `accepted_matches` 1; one more change → 0.
- [ ] **Step 2–4.** **Step 5: Commit** `feat(verify): diff against baseline, accept by hash`.

### Task 8: Disk checks

**Files:**
- Create: `verify_disk.c`, `verify_disk.h`
- Test: `tests/test_verify_disk.c`

**Interfaces:**
- Consumes: `dev_resolve` (Task 6).
- Produces:
  - `typedef enum { FS_LOCAL_REQ, FS_LOCAL_NOFAIL, FS_SWAP, FS_NETWORK, FS_VIRTUAL, FS_BIND, FS_AUTOMOUNT, FS_LAYERED, FS_NOAUTO } fsclass_t;` and `fsclass_t fstab_class(const struct mntent *m);`
  - `int disk_findings(const char *fstab_path, const char *kmsg_text, const char *devdir, finding_t *out, int max);` — FAIL for `FS_LOCAL_REQ` unresolved; WARN for nofail/swap missing; NOTE for network/virtual/bind/automount/layered; WARN per device with `I/O error`, `EXT4-fs error`, `BTRFS … corrupt`/`errs:` in kmsg text.
- [ ] **Step 1: Failing tests**: a fixture fstab with one of each class against a fake devdir; kmsg fixture lines produce WARN naming `sde`.
- [ ] **Step 2–4.** **Step 5: Commit** `feat(verify): classified fstab and kernel-log disk checks`.

### Task 9: `schema-ctl verify` report

**Files:**
- Modify: `schema-ctl.c` (command, help text), `Makefile`
- Create: `verify.c`, `verify.h` (orchestration and printing)
- Modify docs: `docs/operations.md` (new "Before a reboot: schema-ctl verify" section), `README.md` (one line under schema-ctl), `docs/svc-reference.md` (no change unless a key note is needed)
- Test: `tests/test_verify_cli.sh`

**Interfaces:**
- Consumes: Tasks 3, 5–8.
- Produces:
  - `int verify_run(const verify_opts_t *o, FILE *out);` returns the exit code (0/1/2/3, Global Constraints). `typedef struct { int quiet, accept; const char *state_dir, *grubenv, *bls_dir, *grubcfg, *fstab, *kmsg_path, *devdir; } verify_opts_t;` — defaults are the real paths (grubenv's `blsdir=` honoured); each overridable by `SCHEMA_VERIFY_*` env for tests.
  - CLI: `schema-ctl verify [--quiet] [--accept] [--write-baseline [--wait-settled N]]`.
  - Report sections in spec order: Next boot (actual / normal / fallback each with their entry checks), Services, Ways back, Disks, `Result: N fail, N change (accepted|not accepted), N warn`. Tags: `[PASS] [CHANGE] [CHANGE accepted] [WARN] [FAIL] [UNRESOLVED] [note]`. `--quiet` prints only non-PASS lines + Result. `init=` not schema-init → `[PASS] next boot is <init>; services not checked`. Pi (no grubenv/BLS) → boot sections `[note] not applicable`, services checked via `/usr/sbin/schema-init --check` with `cmdline.txt` and `pi_choose_target`; undeterminable → services `[note] not applicable`, not UNRESOLVED (spec §7).
- [ ] **Step 1: Failing `tests/test_verify_cli.sh`** (non-root, all paths via env to fixtures, `schema-init --check` faked by a stub on the root-inspect path): clean → exit 0; unaccepted CHANGE → exit 2, `--accept` → exit 0 and `[CHANGE accepted]`; edit after accept → exit 2; blank-line fallback entry → exit 1; `BASE_LOST` → exit 2; non-root real run (no env) → `[UNRESOLVED] needs root`, exit 3, no crash (Review Focus 2); `--quiet` has no `[PASS]` lines.
- [ ] **Step 2–4.** **Step 5: Commit** `feat(ctl): schema-ctl verify`.

### Task 10: Real-binary regression and VM run

**Files:**
- Create: `tests/test_verify_regression.sh`, `Makefile` target `test-verify-regression` (not in `make test`: it builds two old trees)
- Test: itself

- [ ] **Step 1: Write the script**: `git worktree add` tmp trees at `434dcac` and `v0.4.27`, build `schema-init` in each, run each `--check --root <fixture root with hardening-default=on and the same .svc set>`; feed v0.4.27 output as baseline, `434dcac` output as next into `diff_baseline` (via a tiny test driver) → assert one CHANGE `syscall_deny … default (default)` covering every fixture service without an explicit `syscall_deny`. Remove worktrees on exit.
- [ ] **Step 2: Run** `make test-verify-regression`; expected PASS.
- [ ] **Step 3: VM run with the schema-vmtest skill** (`~/Ocean/vm/isotest/isotest.qcow2`): install the PR build; `schema-ctl verify` → 0 or 2 with sane output; `--write-baseline`; corrupt the fallback entry with a blank `initrd` line → `[FAIL]` under Ways back; restore. Shut the VM down from inside (never hard-kill). Record outputs in the PR description.
- [ ] **Step 4: Commit** `test(verify): 11-18 regression against the real binaries`. Open PR 2 as draft → `/code-review` → fix → CI green → merge.

---

## PR 3 — the gates

### Task 11: Gates in schema-ctl

**Files:**
- Modify: `schema-ctl.c` (`reboot`, `poweroff`, `reexec` accept `--force`; gate before contacting PID 1)
- Create: `verify_gate.c`, `verify_gate.h`
- Test: `tests/test_verify_gate.sh`

**Interfaces:**
- Consumes: `verify_run` (Task 9) — invoked as a child process `schema-ctl verify --quiet` so a crash or hang is contained.
- Produces: `int verify_gate(const char *action, int force, int interactive, FILE *out);` → 0 proceed, 1 refuse. Rules: `force` → proceed. `reboot`/`poweroff` non-interactive → proceed without running verify. Child exit 0 → proceed; 1/2 → refuse with the report and "`--force` to override"; 3 → `reexec` refuses, `reboot`/`poweroff` print the UNRESOLVED lines as `[WARN]` and proceed; crash/timeout (30 s) → `reexec` refuses; `reboot`/`poweroff` print `[WARN] verification unavailable; proceeding with <action>` and proceed. `interactive = isatty(0) && isatty(2)`. Env `SCHEMA_VERIFY_BIN` overrides the child path (tests).
- [ ] **Step 1: Failing tests** with stub verifiers (exit 0 / 1 / 2 / 3 / `kill -SEGV $$` / `sleep 40`), run under `script -qc` for a pty and plain for non-tty, socket path pointed at a nonexistent file so "proceed" is observable as the socket-connect error: interactive+1 → refused, no connect attempt; interactive+3 reboot → WARN then connect attempt; reexec+3 → refused; interactive+1+`--force` → connect attempt; non-tty+1 → connect attempt; reexec+crash → refused; reboot+hang → WARN after 30 s then connect attempt (Review Focus 5).
- [ ] **Step 2–4.** **Step 5: Commit** `feat(ctl): gate reexec and interactive reboot/poweroff on verify`.

### Task 12: System callers that must never block

**Files:**
- Modify: `scripts/schema-pi-deadline` (`"$CTL" reboot --force`), `scripts/schema-dbus-flip-healthcheck.sh` and the udev flip healthcheck (their `schema-ctl reboot` calls get `--force`), `distros/fedora-installer/schema-flip-apply.sh`, any other `grep -rn 'schema-ctl\(") \)\?\(reboot\|poweroff\|reexec\)' scripts distros` hit that runs unattended. **Not** `scripts/schema-pi-halt` (that is how a person's `reboot` arrives on the Pi; it stays gated) and **not** the `systemctl` shim.
- Test: `tests/test_gate_callers.sh`

- [ ] **Step 1: Failing test**: a fixed list of unattended scripts; each `schema-ctl … reboot|poweroff|reexec` call in them carries `--force`; `schema-pi-halt` and `systemctl_shim.h` do not.
- [ ] **Step 2–4.** **Step 5: Commit** `fix(gate): unattended callers pass --force`.

### Task 13: Packaging and snapshot arm

**Files:**
- Create: `scripts/schema-reexec-gated` (shared by both scriptlets: runs `schema-ctl verify --quiet`; exit 0 → `schema-ctl reexec --force`; 1/2/3 → banner `schema-init: PID 1 kept its old binary — schema-ctl verify found problems before the next boot:` + the quiet report + `fix or 'schema-ctl verify --accept', then 'schema-ctl reexec'`; verifier missing/crashed → skip re-exec with "verification unavailable"; always exits 0 so the package transaction never fails)
- Modify: `schema-init.spec` `%posttrans` re-exec call → `schema-reexec-gated`; `debian/schema-init.postinst` trigger branch (line 47) → same; `scripts/schema-snapshot` `cmd_arm`: unless `--force`, run `schema-ctl verify --quiet` and `die` on any non-zero (1/2/3), replacing the grep in `check_armable()` (keep `check_armable` only for the `--force` path's minimal sanity)
- Docs: `docs/boot-success-guard.md` (arm now requires verify), `docs/operations.md` (gates section)
- Test: `tests/test_reexec_gated.sh`, `tests/test_snapshot_arm_gate.sh`

- [ ] **Step 1: Failing tests** with stub `schema-ctl` recording argv: verify 0 → `reexec --force` recorded; verify 2 → no reexec, banner printed, script exit 0; verify missing → no reexec, exit 0; `schema-snapshot arm` with verify 1 → exit non-zero, no `grub2-editenv` call (stubbed); `arm --force` → editenv called.
- [ ] **Step 2–4.**
- [ ] **Step 5: Full `make test`, then the schema-vmtest skill**: upgrade the VM to the PR build via rpm with a deliberately broken `.svc` present → re-exec skipped, banner shown; fix → `schema-ctl reexec` works; `schema-snapshot arm` refuses then passes.
- [ ] **Step 6: Commit** `feat(gate): packaging re-exec and snapshot arm run verify`. Open PR 3 as draft → `/code-review` → fix → CI green → merge. Release notes + spec bump before tag per `[[feedback_bump_spec_before_tag]]`.

---

## Deferred (spec allows)

- `schema-doctor` desktop notification when the last upgrade-time verify failed — follow-up PR after PR 3.
- JSON output.
