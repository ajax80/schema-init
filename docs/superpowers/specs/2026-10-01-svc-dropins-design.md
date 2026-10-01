# .svc Drop-ins

**Status:** Approved 2026-10-01 (Jonathan; Greg review folded in). Open questions decided: defer live apply; bad drop-in fails closed.
**Component:** `service.c` parser (`services_load`, `service_load_one`), `schema-ctl`, `schema-doctor.py`, `schema-systemd1.py`.

## Problem

A `.svc` is one flat file. Changing one knob (`mem_limit=`, a hardening flag, an
extra `env=`) means editing the whole file, which:

- forks it from whatever produced it (`/usr/share/schema-init/services` reference
  copy, or `schema-import` output from an RPM unit). `schema-import` skips files
  that exist, so an edited `.svc` never picks up upstream unit changes again, and
  a re-import after deleting it loses the local edit;
- blocks per-daemon hardening (next roadmap step, Landlock after it) from landing
  as small, reviewable, removable overrides;
- hides what is local: nothing distinguishes "shipped" from "operator changed".

## Design

### 1. Layout

```
/etc/schema-init/services/foo.svc
/etc/schema-init/services/foo.svc.d/10-mem.conf
/etc/schema-init/services/foo.svc.d/50-hardening.conf
```

- Directory is `<file basename>.svc.d`, next to the `.svc`. Read: names ending
  in `.conf` that do not start with `.` (skips editor droppings such as emacs'
  `.#10-mem.conf` lock symlink). Symlinks are followed; the target must be a
  regular file (`stat`, not `d_type`), anything else is skipped with a warning.
  Anything else in the directory is ignored.
- Order is plain `strcmp` of the filename, via a `scandir` comparator — **not**
  `alphasort`, which uses `strcoll`: `schema-ctl cat` runs in the user's locale
  and would otherwise be able to list drop-ins in a different order from the
  one PID 1 (C locale) applied. That one order drives both the merge and the
  hash.
- Template instances: `motor@a.svc` reads `motor@.svc.d/` first, then
  `motor@a.svc.d/`. Two levels, nothing more. The template directory applies
  whether or not a `motor@.svc` file exists (that file is never a spawnable
  service today; instances are their own `motor@a.svc` files).
- A drop-in directory with no matching `.svc` does nothing (no service is
  created from drop-ins alone). `schema-doctor` reports it as orphaned.
- `.grp` files: out of scope.

### 2. Merge semantics

Drop-in lines use exactly the `.svc` syntax and go through the same parser, as
if appended to the end of the base file, in order:

- **Scalar keys** (`exec`, `user`, `mem_limit`, `priority`, `no_new_privs`, …):
  last assignment wins. This is already how the parser behaves within one file.
- **List keys** — `args`, `env`, `dep`: append. An **empty assignment** (`args=`,
  `env=`, `dep=`) clears the list built so far, then later lines append again.
  Same rule as systemd's `ExecStart=` reset, so imported muscle memory works.
  - `exec=` in a drop-in does **not** clear `args`; to replace the command line,
    write `exec=…` then `args=` then the new `args=` lines.
  - A reset frees the `strdup`'d `argv[1..]` / `envp[]` entries before zeroing
    the count (PID 1 and `schema-ctl add` both parse; a leak per reset per
    reload adds up). `argv[0]` is `svc->exec`, not heap, and is kept.
  - `dep=` names are resolved to indices in the second pass of
    `services_load`, after every file and drop-in is parsed, and cycle checks run
    after that; a drop-in that clears and rebuilds `dep=` is seen in full by
    both.
- `name=` in a drop-in is rejected (the service's identity comes from the base
  file / filename; a drop-in renaming it would break `dep=` resolution silently).
- Flag keys that today only set on true (`oneshot`, `needs_root`, `critical`,
  `no_restart`, `persistent`): in a drop-in they set **and clear**, so
  `no_restart=0` can undo the base.
- **Base files parse exactly as before.** The empty-assignment reset, flag
  clearing and the `name=` refusal apply only to lines read from a drop-in.
  Found while implementing: every fleet host has base files full of `critical=0`
  / `oneshot=0`, and a timer whose `on_boot_sec=` sets `SVC_ONESHOT` followed by
  `oneshot=0` would have changed meaning if clearing applied to base files too.
- A bad line in a drop-in (`exec=` with whitespace, unknown cap, bad ns field)
  rejects the whole service, same as a bad line in the base, with the drop-in's
  path in the message.

### 3. One parse chain

`services_load` and `service_load_one` carry duplicated parse chains today
(`/* keep in sync */`). Drop-ins make that worse, so first:

- Extract `svc_parse_file(service_t *svc, const char *path, struct parse_ctx *pc)`
  where `parse_ctx` holds `argc`, `dep_slot`, `bad`, and the running hash.
  The line loop and every key handler move into it once.
- `svc_init_defaults(svc)` (the memset + defaults block) and
  `svc_finalize(svc, path)` (hardening_finalize, cpuset/persist warnings, start
  timeout default, name-from-filename, template skip) are shared too.
- `services_load` = for each `.svc`: defaults → parse base → parse drop-ins →
  finalize. `service_load_one` (`schema-ctl add`) does the same, so a runtime-added
  service gets its drop-ins too.

This refactor lands as its own commit with no behaviour change, verified by the
existing parser tests plus a new golden test (every reference `.svc` in
`services/` parsed before and after, `service_t` fields compared).

### 4. Integrity hash covers drop-ins

`content_hash` becomes one running FNV-1a: the base file's bytes (exactly
today's `fnv1a_file`), then for each drop-in read, its path, a NUL, its bytes.
With no drop-ins the hash is unchanged (see Rollout). Consequences, all deliberate:

- Adding, removing, renaming or editing a drop-in is "modified since boot":
  `schema-ctl reload` and `schema-ctl reexec` refuse exactly as they do for an
  edited `.svc` today. **A drop-in takes effect at next boot**, same contract as
  the base file. (Live apply is a separate decision, see Open questions.)
- Including the path means moving a drop-in between `foo.svc.d` and
  `foo@.svc.d` changes the hash even when the bytes are identical.
- The `%posttrans` auto-reexec will report the refusal (it never fails the
  transaction), so an operator who added a drop-in and then ran `dnf upgrade`
  sees why PID 1 was not replaced. The reexec state blob carries `content_hash`
  already; no format change.

### 5. Seeing the result: `schema-ctl cat <name>`

New verb, read-only, no PID 1 involvement (runs in the client):

```
# /etc/schema-init/services/foo.svc
exec=/usr/bin/foo
…
# /etc/schema-init/services/foo.svc.d/10-mem.conf
mem_limit=512
```

Prints the base and each drop-in in the order applied, headed by their paths.
Uses the same directory-resolution helper as the parser (shared header), so
`cat` cannot disagree with what PID 1 loaded. Exit 1 if no such `.svc`.

### 6. Python readers

- `schema-doctor.py` `hardening_unannotated()`: a knob set in any drop-in counts
  as annotated. New check `orphan-dropins`: `*.svc.d` with no `.svc` → warning.
- `schema-systemd1.py` `_read_can_reload()`: read base + drop-ins, last
  `reload=` wins. `FragmentPath` stays the base; add `DropInPaths` (systemd's
  property name) with the list, so `systemctl status` via the shim shows them.
- One small shared Python helper (`svc_files(name)` → ordered path list) in each
  script; they do not share a module today and this does not introduce one.

## Not doing

- `/usr/lib/schema-init/services` vendor layer, `/run` layer, or masking. Fleet
  has one config dir; adding precedence layers is a bigger change with no current
  user.
- `schema-ctl edit` (systemctl-edit style). `cat` + an editor is enough for now.
- Live apply of a changed service (see Open questions).
- Drop-ins for `.grp`.

## Testing

1. **Parser unit tests** (`tests/test_svc_dropins.c`): scalar override, list
   append, empty-assignment reset for each list key, `exec=` not clearing args,
   flag clear (`no_restart=0`), `name=` rejected, bad drop-in line rejects the
   service, ordering (`10-` before `50-`, non-`.conf` ignored), template then
   instance dir, orphan dir ignored, hash changes on add/remove/edit/move.
2. **Golden no-change test** for the refactor commit (§3).
3. **ASan/UBSan** run of the parser tests (argv/env strdup + reset frees).
4. **vmtest as PID 1**: boot with a drop-in that sets `mem_limit` and an `env=`;
   verify cgroup `memory.max` and `/proc/<pid>/environ`. Then add a drop-in and
   confirm `reload` and `reexec` both refuse with the "modified" message.
5. **blakbox HW**: one real override (candidate: a `mem_limit` drop-in on a
   peripheral service), reboot, verify; `schema-ctl cat` output checked by eye.

## Rollout

Two PRs, both drafts until `/code-review`:

1. Parser refactor (§3), no behaviour change. COPR → fleet via `dnf upgrade`
   (auto re-exec; hashes unchanged because no drop-ins exist yet — verify the
   re-exec is not refused on all 4 hosts).
2. Drop-ins + hash + `cat` + Python readers. Same rollout. Existing hosts have no
   `.svc.d` directories, so the new hash equals the old one for every service
   only if the base-only hash is computed the old way. **Requirement:** with zero
   drop-ins, `content_hash` must equal today's `fnv1a_file(path)` — no path
   prefix for the base file, path+NUL prefix only for drop-ins. Otherwise the
   first auto re-exec after upgrade is refused fleet-wide as "modified". Test this
   explicitly.

## Open questions

1. **Live apply.** Today any changed `.svc` waits for a reboot (the integrity
   check). With re-exec available, a `schema-ctl reload --accept <name>` (explicit,
   per service, logged) would make drop-ins usable without a reboot. You chose
   "refuse" for re-exec on 09-30; recommend keeping it out of this spec and
   deciding after drop-ins are in use.
2. **Bad drop-in = service rejected** (§2) matches base-file behaviour but means
   a typo in an override can keep a service from starting at boot. Alternative:
   skip the bad drop-in, load the rest, warn. Recommend matching the base (fail
   closed) — a silently skipped hardening drop-in is worse than a loud failure.
