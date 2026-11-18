# supplementary_groups= / dynamic_user= (audit 4.10) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Imported units keep their privilege model: supplementary groups apply, and `DynamicUser=yes` runs as an allocated uid with isolated state instead of root.

**Architecture:** Keys parse in service.c; a pure `dynuser.h` picks the uid; PID 1 allocates before the spawn and carries it; the child sets groups/uid in both the main and runner drop paths, prepares `<base>/private/<dir>` on the host, and inside the mount namespace hides `private/` behind a tmpfs with only its own dirs bound back (O_PATH held first).

**Tech Stack:** C99 static PID 1; Python 3 importer.

**Spec:** `docs/superpowers/specs/2026-11-17-groups-dynuser-design.md`

## Global Constraints
- `supplementary_groups=`: names or gids, ≤16 total, later lines add, empty clears; unknown name → exit 126 `UID DROP FAILED for <svc>: supplementary_groups: <name>`; buffer getgroups(0)+16, deduplicated; applies to root services; same in svc_runner_drop.
- `dynamic_user=1`: static `user=` that exists wins (allocation skipped, everything else applies); range 61184..65519, start 61184 + FNV-1a(name) % 4336, probe up with wrap, skip getpwuid/getgrgid hits and uids other loaded services hold; none free → start refused; gid = uid; carried in SVC_RUNTIME_FIELDS (`X(INT, dyn_uid)`).
- Implied unless explicit: no_new_privs=1, private_tmp=1, protect_system=full, protect_home=1.
- Env: USER/LOGNAME = service name, HOME=/ (allocated uid only).
- State/cache/logs: `<base>/private` root 0700; `<base>/private/<dir>` owned uid; `<base>/<dir>` symlink `private/<dir>`; plain pre-existing `<base>/<dir>` renamed into private first; wrong owner → hardened recursive chown (O_NOFOLLOW|O_DIRECTORY, fchownat AT_SYMLINK_NOFOLLOW, no st_dev crossing). In-ns: O_PATH each own dir, tmpfs 0755 on `<base>/private`, mkdir + bind back via /proc/self/fd.
- schema-user: dynamic_user ignored with one log line.
- No docstrings; why-comments only.

## Review Focus
1. A dynamic service whose state dir holds a symlink to /etc/shadow, after a uid change → the chown leaves /etc/shadow's owner untouched. Test: Task 4 root test `chown_no_follow`.
2. Two dynamic services hashing to the same start → different uids, both stable across reload. Test: Task 3 `test_dynuser` probe + Task 5 live.
3. A service with only `supplementary_groups=` (root) gets the extra groups, not an empty list. Test: Task 2 root test.
4. fwupd-refresh's existing plain `/var/cache/fwupdmgr` keeps its contents after conversion. Test: Task 4 root test `migrate_plain_dir`.
5. Another dynamic service can't read the first's state even with the same uid number. Test: Task 5 live (namespace view lists only its own dir under private/).

---

### Task 1: keys and implied hardening
**Files:** `service.h` (`char supp_groups[16][64]; uint8_t supp_count; uint8_t dynamic_user; int dyn_uid;`), `service.c` (parser, hardening_finalize implies), `reexec_state.h` (`X(INT, dyn_uid)`), `tests/test_svc_dynuser.c` + Makefile.
- [ ] tests: `supplementary_groups=wheel 10` → 2; 17 → refused; drop-in empty clears; `dynamic_user=1` → no_new_privs/private_tmp=1/protect_system=2/protect_home=1 with hard_source "default"? (implied, shown as `dynamic_user`), explicit `private_tmp=0` wins; `dynamic_user=2` refused.
- [ ] RED → implement → GREEN (`make test`) → commit.

### Task 2: groups in the drop
**Files:** `service.c` (`static void svc_supp_groups_or_die(const service_t *svc, gid_t gid)` called after initgroups in the main child and svc_runner_drop; root path calls it alone when supp_count), `tests/test_supp_groups.c` (root-only; skips otherwise) + Makefile (and a `sudo -n` run note like mountns-rw).
- [ ] tests (as root, fork a probe via service_spawn with `exec=/bin/sh -c 'id -G > out'`): `user=nobody supplementary_groups=<gid of "adm" or 4>` → id -G contains it and nobody's own; root service with `supplementary_groups=<gid>` → contains it; unknown name → exit 126; exec_stop runner sees the same groups.
- [ ] RED → implement → GREEN (`sudo -n build/test/supp-groups` + make test) → commit.

### Task 3: uid allocation
**Files:** create `dynuser.h` (`static inline uid_t dynuser_pick(const char *name, int (*taken)(uid_t, void *), void *ctx)` → 0 if none); `engine.c` (allocate before spawn when `dynamic_user && dyn_uid == 0 && !static user`, taken = getpwuid||getgrgid||another service's dyn_uid; status `  dynamic uid %d`); `service.c` child uses dyn_uid as run_uid/run_gid and sets USER/LOGNAME/HOME; `tests/test_dynuser.c`.
- [ ] tests: start = 61184 + fnv % 4336 for a known name (assert value); taken start → next; taken up to 65519 wraps to 61184; all taken → 0.
- [ ] RED → implement → GREEN → commit.

### Task 4: private dirs
**Files:** `service.c` (`int service_chown_tree(int dfd, uid_t, gid_t, dev_t)`; host-side prep in svc_make_exec_dirs when dynamic: private parent, migrate, symlink, owner fix), `ns.c`/`ns.h` (apply_mount_ns takes `const char *const *priv_base, const char *const *priv_dirs, int n_priv` — O_PATH, tmpfs, rebind), `tests/test_dynuser_dirs.c` (root-only).
- [ ] tests (root, temp base via a test hook env `SCHEMA_EXEC_DIR_BASE_TEST` refused unless root, like the udev test feed): `chown_no_follow` (tree with symlink to an outside file and a nested bind-free dir; outside file owner unchanged, inner files re-owned); `migrate_plain_dir` (plain dir with a file → private/<dir>/file, symlink in place); `ns_view` (in an unshare'd child: tmpfs over private, own dir readable through the symlink, a sibling private dir absent).
- [ ] RED → implement → GREEN → commit.

### Task 5: importer, docs, PR, review, live
- [ ] importer tests: `SupplementaryGroups=a b` → `supplementary_groups=a b`; `DynamicUser=yes` (earlyoom shape) → `dynamic_user=1`, no "runs as root" warning; with `User=wsdd` → both lines; schema-user imports drop dynamic_user.
- [ ] docs: svc-reference rows + a Dynamic users section (NSS note), roadmap, audit spec 4.10 done.
- [ ] draft PR; one `/code-review`; fix pass RED→GREEN; suites green.
- [ ] Live blakbox PID 1: re-exec into branch; a dynamic test svc with state_dir + supplementary_groups: uid in range, `id -G`, writes state, `/var/lib/<d>` symlink, private 0700; second dynamic svc can't see the first's dir; re-exec keeps the uid; cleanup (remove `/var/lib/private/<test dirs>` and symlinks), back to packaged, 67 healthy.
- [ ] merge on Jonathan's word; deb.yml green.
