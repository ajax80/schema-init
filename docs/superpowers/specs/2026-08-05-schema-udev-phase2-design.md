# schema-udev Phase 2 — coldplug + declarative symlinks

**Status:** design approved 2026-08-05
**Predecessor:** [schema-udev Phase 1](2026-08-05-schema-udev-phase1-design.md) — native uevent→schema→action daemon, deployed live alongside real systemd-udevd (netlink group 1 only).
**Roadmap context:** Tier-2 reclamation flagship. Phase 2 does **not** retire real udevd — that is Phase 3 (libudev db + group-2 rebroadcast).

## Goal

Give schema-udev two capabilities it lacks after Phase 1, both safe to run alongside the live desktop's real systemd-udevd:

- **A. Coldplug** — fire rules for devices *already present* at daemon startup, not only on future hotplug events.
- **B. Declarative symlinks** — let a `.dev` rule declare a stable name for a device under a parallel `/dev/schema/` namespace, created on add and removed on remove.

## Non-goals (explicit — deferred to Phase 3 or dropped)

- Contesting real `/dev` (e.g. `/dev/serial/by-id/`). schema-udev writes **only** under `/dev/schema/`; real udevd's namespace is untouched.
- `by-id` / `by-uuid` symlink farms. No sysfs vendor/model/serial crawl, no udev string-encoding reimplementation, no libblkid. Names are explicitly declared by the rule.
- Parent-chain attribute inheritance for matching. `match_product` on a bare `tty` node still only works if the kernel placed `PRODUCT` on that node's event — unchanged from Phase 1.
- Writing to `/sys/*/uevent` for coldplug. That triggers a **global** kernel rebroadcast (real udevd reprocesses, re-applies ACLs, rebroadcasts on group 2 to the whole desktop) — the exact hazard Phase 1 forbade. Coldplug is an internal sysfs read only.
- libudev database, `/run/udev` state, group-2 monitor rebroadcast. All Phase 3.

## The safety invariant (unchanged from Phase 1, restated)

schema-udev must never cause the real systemd-udevd to reprocess a device or emit on group 2. Everything in Phase 2 upholds this:
- Coldplug reads sysfs `uevent` files directly and dispatches in-process. It never writes `/sys` and never touches netlink.
- Symlinks live under `/dev/schema/`, a namespace no other daemon manages, so there is no writer contention with udevd.

## Feature A — Coldplug via internal sysfs walk

### When
Once per process, at startup: after the initial `rules_reload()`, before entering the `poll()` loop. A supervisor respawn re-runs it (acceptable — matches udev's boot-time `udevadm trigger` semantics; at cold start there was no prior live event to double-fire against). A `SIGHUP` rule reload does **not** re-coldplug — it only reloads rules.

### Mechanism
Walk `/sys/devices` with `nftw(..., FTW_PHYS)` (physical walk — never follow symlinks, so no cycles). For every regular file named `uevent`, process its parent directory `D`:

1. Start an empty `struct uevent`.
2. Set `ACTION=add`.
3. Set `DEVPATH=` the path of `D` with the leading `/sys` stripped (e.g. `/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/ttyUSB0`). This matches the kernel's live `DEVPATH` convention (relative to sysfs root, starting with `/devices/`).
4. Set `SUBSYSTEM=` the basename of `readlink(D + "/subsystem")` if that symlink exists (e.g. `.../class/tty` → `tty`). If absent, omit SUBSYSTEM (intermediate nodes legitimately have none).
5. Append every `KEY=VALUE` line from `D/uevent` (newline-separated, no header line, no trailing NUL — unlike the netlink wire format). This supplies DEVNAME, MODALIAS, MAJOR, MINOR, and whatever else the kernel exposes for that node.
6. Run the completed event through the existing `dispatch()`.

Because dispatch only acts on a rule match, unmatched nodes cost one `uevent` file read and nothing else. On a typical desktop this is a few thousand small reads at startup — effectively instant.

### Fidelity
Coldplug reproduces live per-node semantics exactly: it presents to `dispatch()` only the keys actually present in that node's `uevent` file (plus the synthesized ACTION/DEVPATH/SUBSYSTEM the kernel would also supply live). A rule that would not match a device's live `add` will not match its coldplug event either.

### New helper
`uevent_from_sysfs(const char *dirpath, struct uevent *ev)` — builds the struct per steps 1–5 above. Lives in `schema-udev.h` alongside the existing FS-touching `dev_rule_load_file`, so tests can drive it against a fabricated `/tmp` sysfs tree. Returns 0 on success, -1 if the `uevent` file cannot be read.

## Feature B — Declarative symlinks under /dev/schema/

### Rule grammar addition
New key `symlink=<name>` in `.dev` files:

```
# esp32.dev
name=esp32-serial
match_subsystem=tty
match_product=10c4/ea60
symlink=esp32
on_add=/usr/local/bin/notify-esp32-up
```

- New field `char symlink[64]` in `struct dev_rule`.
- Set in `dev_rule_set` under key `symlink`, with **validation**: reject empty, any name containing `/` or `..`, or longer than 63 chars. A rejected value returns the unknown-key error path (warning printed, that rule's symlink stays unset — the rest of the rule still loads). This keeps the created path confined to a single entry directly under `/dev/schema/`.

### Lifecycle
Base directory `#define SCHEMA_DEV_DIR "/dev/schema"`.

- **On `add`**, when the matched rule has `symlink` set and the event carries `DEVNAME`:
  1. `mkdir(SCHEMA_DEV_DIR, 0755)` if it does not exist.
  2. Atomically create `/dev/schema/<name>` → `/dev/<DEVNAME>`: `symlink()` to a temp name in the same dir, then `rename()` over the final name. Atomic replace survives rapid replug races without a window where the link is missing.
  3. *Then* run `on_add` — the hook can rely on the symlink already existing.
  - If `DEVNAME` is absent, skip symlink creation (nothing to point at) and still run the hook.
- **On `remove`**, when the matched rule has `symlink` set:
  1. `unlink(/dev/schema/<name>)`, ignoring `ENOENT`.
  2. *Then* run `on_remove`.
  - Symmetric with add: manage the link first, then the hook.

Target is the absolute path `/dev/<DEVNAME>` (DEVNAME on group 1 / in sysfs has no `/dev/` prefix).

### Testability seam
The create/remove operations are implemented as `symlink_apply(base, name, devname)` and `symlink_clear(base, name)` taking an explicit base directory, so tests exercise them against a `/tmp` base without root or a real `/dev`. Name validation is pure (in `dev_rule_set`) and tested directly.

## Files

- **Modify `schema-udev.h`**
  - Add `char symlink[64];` to `struct dev_rule`.
  - Handle `symlink` key + validation in `dev_rule_set`.
  - Add `uevent_from_sysfs(dirpath, ev)`.
  - Add pure symlink path/target builders as needed by the apply/clear functions.
- **Modify `schema-udev.c`**
  - `coldplug_walk(void)` using `nftw` — synthesize + dispatch per Feature A.
  - `symlink_apply(base, name, devname)` / `symlink_clear(base, name)`.
  - Wire symlink apply/clear into `dispatch()` around the hook (add: apply→hook; remove: clear→hook).
  - Call `coldplug_walk()` in `main()` after `rules_reload()`, before the poll loop.
- **Create `tests/test_symlink.c`** — name validation (accept `esp32`; reject ``, `a/b`, `..`, 64-char); `symlink_apply` creates a correct link against a `/tmp` base and mkdir's the base; atomic overwrite replaces an existing link; `symlink_clear` removes it and is a no-op on a missing link.
- **Create `tests/test_coldplug.c`** — build a fake `/tmp/sys/devices/.../ttyX/` tree with a `uevent` file and a `subsystem` symlink; assert `uevent_from_sysfs` yields ACTION=add, the correct DEVPATH, SUBSYSTEM from the link, and the file's keys; assert a matching rule dispatched from it fires.
- **Modify `assets/example.dev`** — add a commented `symlink=` example line (file stays inert: all lines commented).
- **Modify `Makefile`** — add the two new test binaries to the test target.
- **Modify `README.md`** — document `symlink=`, the `/dev/schema/` parallel namespace, and coldplug-at-startup behavior.

## Testing strategy

- **Unit (pure/FS-against-tmp, no root, no netlink):** all of `test_symlink.c` and `test_coldplug.c`, plus the existing `test_uevent_parse` / `test_dev_match` / `test_dev_load` stay green. This is the primary gate — Phase 2's new logic is fully covered without hardware.
- **vmtest:** `~/schema-livetest/vmtest.sh` must still PASS (schema-udev is not part of the PID-1 boot rail, but the build must stay clean and the box must boot).
- **Live smoke (post-merge, on blakbox):** author a real `.dev` with `symlink=` for a bench device, redeploy the binary, replug, confirm `/dev/schema/<name>` appears and is removed on unplug; confirm coldplug fires for an already-present matching device on restart. Confirm no group-2 / desktop disturbance (PipeWire, KDE) throughout.

## Deployment

Unchanged from Phase 1: schema-udev is a supervised child service, **not** the PID-1 binary → no reboot. Rebuild, `sudo install` the binary to `/usr/bin/`, `kill` the running child so the supervisor respawns it on the new binary. `/dev/schema/` is created lazily on first symlink-bearing add.

## Success criteria

1. A device present before schema-udev starts fires its rule's `on_add` (and gets its symlink) via coldplug.
2. A rule with `symlink=esp32` produces `/dev/schema/esp32 → /dev/ttyUSB0` on plug and removes it on unplug, atomically.
3. Bad `symlink=` values are rejected at load with a warning; the rest of the rule still loads.
4. All unit tests pass; vmtest PASS; no regression in Phase 1 behavior.
5. No group-2 traffic, no udevd reprocessing, no desktop-hotplug disturbance — the safety invariant holds.
