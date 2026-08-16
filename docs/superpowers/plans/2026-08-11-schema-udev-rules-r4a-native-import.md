# schema-udev R4a — Native IMPORT / TEST + RUN-record Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give schema-udev's shadow rule interpreter native `TEST`, native `IMPORT{cmdline,db,parent,builtin}`, and RUN-recording — closing the pure-native half of R4 so the superset (GOTCHA #2) shrinks to only shell-bridged residue.

**Architecture:** Header-only additions to `udev_ruleset.h` plus a behavior-preserving extract-function refactor of `udev_builtins.h`. TEST becomes a real match clause in `rule_match`; the four native IMPORT subtypes and RUN dispatch from `apply_rule`. IMPORT{builtin} reuses the existing (currently caller-less) `run_builtins` dispatcher by extracting a single-builtin `run_builtin_bit`. All shadow-only: real udevd stays authoritative; nothing deployed, no reboot; `schema-udev.c` stays byte-identical.

**Tech Stack:** C11-clean / C99-compiled single-file headers, `assert`-based test binaries built by the `test` Makefile target, POSIX `stat`/`fopen`. Reuses `udev_db.h`, `udev_builtins.h`, `path_id.h`, `schema-udev.h`.

## Global Constraints

- Branch: `feat/schema-udev-cutover-e3` (unpushed WIP; shadow-until-R5). Do **not** push.
- `schema-udev.c` **byte-identical** to prior — verify with `git diff HEAD -- schema-udev.c` empty at the end. R4a is `udev_ruleset.h` + `udev_builtins.h` + tests only.
- Zero warnings under **both** `-std=c99` (Makefile) **and** `-std=c11`, each with `-Wall -Wextra -D_GNU_SOURCE -I.`. **The Makefile builds at `-O2` (`CFLAGS ?= -O2`)** — `-Wformat-truncation` and other optimization-gated diagnostics only fire with optimization on, so verify at **`-O2`** (matching `make test`) AND at `-O0`; the Makefile only gates c99, so compile c11 explicitly.
- Changing a match clause's semantics (e.g. TEST becoming a resolved gate) can invalidate an existing test that encoded the old behavior — `tests/test_udev_executor.c` asserts TEST-as-deferred-superset (R3). Whichever task changes TEST must update that test to the new behavior, or the suite goes red.
- No new external deps. No docstrings/comments beyond what already exists in these headers.
- Live box UNTOUCHED: no `install`, no daemon restart, no `/dev` or `/run/udev/data` writes. IMPORT{db}/{parent}/{cmdline} read live state **read-only** and only in the daemon; tests use the `dbroot`/`cmdline_path` seams pointed at tmp trees.
- Test idiom (match existing `tests/test_udev_executor.c`): single `int main(void)`, `assert(...)`, `printf("test_udev_r4a: <section> OK\n")`, local `ue_set` helper, `ruleset_parse_line`.

## File Structure

- `udev_ruleset.h` — MODIFY. `dev_ctx` growth; `test_clause_match` + wire into `match_dev_clause`/`rule_match`; `apply_import` + `import_cmdline`/`import_db`/`import_parent`/`builtin_name_bit`; `ctx_add_run`; `apply_rule` IMPORT/RUN dispatch + hard-gate early return; `ruleset_apply` deferred-accounting reorder.
- `udev_builtins.h` — MODIFY. Extract `run_builtin_bit(sysroot,devpath,devnode,ev,bit)` returning builtin status; `run_builtins` re-expressed as a loop over it (observable behavior unchanged).
- `tests/test_udev_r4a.c` — CREATE. All R4a unit tests.
- `Makefile` — MODIFY. One line under `test:` building `test_udev_r4a.c`.

---

### Task 1: dev_ctx growth (dbroot, cmdline_path, runs)

**Files:**
- Modify: `udev_ruleset.h` (`struct dev_ctx` ~line 205; `dev_ctx_init` ~line 214)
- Test: `tests/test_udev_r4a.c` (create)
- Modify: `Makefile` (`test:` target)

**Interfaces:**
- Produces: `dev_ctx.dbroot` (`const char *`, defaults `"/run/udev/data"`), `dev_ctx.cmdline_path` (`const char *`, defaults `"/proc/cmdline"`), `dev_ctx.runs[DEVCTX_RUNS_MAX][UE_VAL_MAX]` + `dev_ctx.nruns` (`int`), `DEVCTX_RUNS_MAX` (32). `dev_ctx_init` sets the two path defaults.

- [ ] **Step 1: Write the failing test** — create `tests/test_udev_r4a.c`:

```c
#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    /* Task 1: ctx defaults */
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add");
    ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(strcmp(ctx.dbroot, "/run/udev/data") == 0);
    assert(strcmp(ctx.cmdline_path, "/proc/cmdline") == 0);
    assert(ctx.nruns == 0);
    printf("test_udev_r4a: ctx-defaults OK\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile line** — under the `test:` target, after the `test_udev_executor.c` line, add:

```make
	$(CC) $(CFLAGS) tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd /home/ajax80/projects/schema-init && cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: compile FAIL — `dev_ctx` has no member `dbroot`.

- [ ] **Step 4: Grow `struct dev_ctx`** — in `udev_ruleset.h`, add before `#define DEVCTX_TAGS_MAX 32` (or alongside the other DEVCTX maxes):

```c
#define DEVCTX_RUNS_MAX 32
```

and inside `struct dev_ctx`, after `int deferred_applies;`:

```c
    char runs[DEVCTX_RUNS_MAX][UE_VAL_MAX];
    int  nruns;
    const char *dbroot;
    const char *cmdline_path;
```

- [ ] **Step 5: Set defaults in `dev_ctx_init`** — after `ctx->sysroot = sysroot;` (init memsets to 0, so `nruns` is already 0):

```c
    ctx->dbroot = "/run/udev/data";
    ctx->cmdline_path = "/proc/cmdline";
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: ctx-defaults OK`.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c Makefile
git commit -m "feat(schema-udev): R4a dev_ctx growth (dbroot, cmdline_path, runs)"
```

---

### Task 2: TEST match clause + re-gate

**Files:**
- Modify: `udev_ruleset.h` (`match_dev_clause` ~line 342; `rule_match` deferred path ~line 424)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Consumes: `ruleset_subst`, `rk_cmp`, `dev_ctx`.
- Produces: `test_clause_match(const struct rule_clause *c, const struct dev_ctx *ctx)` → 1 match / 0 no-match. `match_dev_clause` now returns 0/1 for `TEST` (no longer `-1`). `rule_match` no longer flags `TEST` deferred.

- [ ] **Step 1: Write the failing test** — append to `main()` before `return 0;`:

```c
    /* Task 2: TEST */
    {
        char dir[] = "/tmp/r4a_testXXXXXX";
        assert(mkdtemp(dir) != NULL);
        char present[PATH_MAX], mode0700[PATH_MAX];
        snprintf(present, sizeof present, "%s/here", dir);
        snprintf(mode0700, sizeof mode0700, "%s/priv", dir);
        FILE *f = fopen(present, "w"); assert(f); fclose(f);
        f = fopen(mode0700, "w"); assert(f); fclose(f);
        assert(chmod(mode0700, 0700) == 0);

        struct uevent tev; memset(&tev, 0, sizeof tev);
        ue_set(&tev, "ACTION", "add"); ue_set(&tev, "DEVPATH", "/devices/x");
        struct dev_ctx tc; assert(dev_ctx_init(&tc, &tev, "/sys") == 0);

        struct rule r;
        char line[256];
        snprintf(line, sizeof line, "TEST==\"%s\"", present);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* exists */

        snprintf(line, sizeof line, "TEST!=\"%s/absent\"", dir);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* absent, != */

        snprintf(line, sizeof line, "TEST==\"%s/absent\"", dir);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 0);   /* absent, == */

        snprintf(line, sizeof line, "TEST{0700}==\"%s\"", mode0700);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 1);   /* mode matches */

        snprintf(line, sizeof line, "TEST{0070}==\"%s\"", mode0700);
        ruleset_parse_line(line, &r); assert(rule_match(&r, &tc) == 0);   /* group bits absent */

        /* re-gate: a TEST-gated rule no longer flags deferred */
        snprintf(line, sizeof line, "TEST==\"%s\", ENV{X}=\"1\"", present);
        ruleset_parse_line(line, &r);
        tc.last_rule_deferred = 0;
        assert(rule_match(&r, &tc) == 1);
        assert(tc.last_rule_deferred == 0);

        unlink(present); unlink(mode0700); rmdir(dir);
    }
    printf("test_udev_r4a: TEST OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: FAIL — `TEST==` currently falls to `-1`/deferred; `rule_match` returns 1 for the absent-`==` case (over-match) → the `== 0` assert fires.

- [ ] **Step 3: Add `test_clause_match`** — in `udev_ruleset.h`, above `match_dev_clause`:

```c
/* TEST{<octal>}=="path" / != : $-subst the path, stat it; optional mode mask. */
static inline int test_clause_match(const struct rule_clause *c, const struct dev_ctx *ctx) {
    char path[PATH_MAX];
    ruleset_subst(c->val, ctx, path, sizeof path);
    struct stat st;
    int exists = (stat(path, &st) == 0);
    if (exists && c->subkey[0]) {
        unsigned mode = (unsigned)strtoul(c->subkey, NULL, 8);
        exists = ((st.st_mode & 07777u & mode) == mode);
    }
    return (c->op == OP_MATCH_NE) ? !exists : exists;
}
```

- [ ] **Step 4: Wire TEST into `match_dev_clause`** — add before the final `return -1;`:

```c
    if (!strcmp(c->key, "TEST"))  return test_clause_match(c, ctx);
```

- [ ] **Step 5: Add the `<sys/stat.h>` include + stop flagging TEST deferred** — `udev_ruleset.h` includes `string.h/stdlib.h/stdio.h/dirent.h` but **not** `sys/stat.h`; add it near the top (after `path_id.h`):

```c
#include <sys/stat.h>   /* TEST clause: stat, st_mode */
```

(`strtoul` is already available via the existing `<stdlib.h>`.) The `d == -1` fallthrough in `rule_match` sets `last_rule_deferred = 1` for unknown match keys; TEST now returns 0/1 from `match_dev_clause`, so it never reaches that branch — no other change needed (confirm nothing special-cases `"TEST"` in the deferred path; it does not).

- [ ] **Step 6: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: TEST OK`.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c
git commit -m "feat(schema-udev): R4a TEST match clause (stat + octal mode), un-defer superset"
```

---

### Task 3: Extract `run_builtin_bit` from `run_builtins`

**Files:**
- Modify: `udev_builtins.h` (`run_builtins` ~line 140)
- Test: `tests/test_udev_r4a.c` (behavior parity), existing `tests/test_udev_builtins.c` still passes

**Interfaces:**
- Produces: `int run_builtin_bit(const char *sysroot, const char *devpath, const char *devnode, struct uevent *ev, int bit)` — runs the single builtin for `bit`, absorbing its props into `ev`; returns the builtin's status (`0` ran / `< 0` failed). `run_builtins` unchanged externally.

- [ ] **Step 1: Write the failing test** — append to `main()`:

```c
    /* Task 3: run_builtin_bit exists and is a no-op on a bogus device */
    {
        struct uevent bev; memset(&bev, 0, sizeof bev);
        ue_set(&bev, "ACTION", "add"); ue_set(&bev, "DEVPATH", "/devices/none");
        int before = bev.n;
        int rc = run_builtin_bit("/nonexistent-sysroot", "/devices/none", NULL, &bev, UB_USB);
        assert(rc < 0);            /* usb_id on a non-USB/absent device fails */
        assert(bev.n == before);   /* nothing absorbed */
    }
    printf("test_udev_r4a: run_builtin_bit OK\n");
```

Add `#include "../udev_builtins.h"` to the test's includes (after `udev_ruleset.h`).

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: compile FAIL — `run_builtin_bit` undefined.

- [ ] **Step 3: Extract the function** — in `udev_builtins.h`, add above `run_builtins`, moving each per-bit body out of the `sel &` blocks. Each builtin's own return value is captured; ports that return length (`path_id_build` > 0 = ok) map to 0/`-1`:

```c
static inline int run_builtin_bit(const char *sysroot, const char *devpath,
                                  const char *devnode, struct uevent *ev, int bit) {
    struct uevent tmp;
    switch (bit) {
    case UB_HWDB:  tmp.n = 0; { int r = hwdb_build(sysroot, devpath, &tmp);      ub_absorb(ev, &tmp); return r; }
    case UB_PATH: {
        char idpath[PATH_ID_MAX], idtag[PATH_ID_MAX];
        if (path_id_build(sysroot, devpath, idpath, sizeof idpath) > 0) {
            ub_add(ev, "ID_PATH", idpath);
            if (path_id_tag(idpath, idtag, sizeof idtag) == 0) ub_add(ev, "ID_PATH_TAG", idtag);
            return 0;
        }
        return -1;
    }
    case UB_USB:   tmp.n = 0; { int r = usb_id_build(sysroot, devpath, &tmp);    ub_absorb(ev, &tmp); return r; }
    case UB_INPUT: tmp.n = 0; { int r = input_id_build(sysroot, devpath, &tmp);  ub_absorb(ev, &tmp); return r; }
    case UB_NET:   tmp.n = 0; { int r = net_id_build(sysroot, devpath, &tmp);    ub_absorb(ev, &tmp); return r; }
    case UB_V4L:   tmp.n = 0; { int r = v4l_id_build(sysroot, devpath, devnode, &tmp);   ub_absorb(ev, &tmp); return r; }
    case UB_ATA:   tmp.n = 0; { int r = ata_id_build(sysroot, devpath, devnode, &tmp);   ub_absorb(ev, &tmp); return r; }
    case UB_BLKID: {
        tmp.n = 0; int rpt = blkid_pt_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp);
        tmp.n = 0; int rfs = blkid_fs_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp);
        return (rpt == 0 || rfs == 0) ? 0 : -1;
    }
    case UB_CDROM: tmp.n = 0; { int r = cdrom_id_build(sysroot, devpath, devnode, &tmp); ub_absorb(ev, &tmp); return r; }
    default: return -1;
    }
}
```

Verify the actual return types of each `*_build` in their headers as you move them; if any returns something other than `int`/`ssize_t` with `0 == ok`, adapt the mapping so `0` = ran and `< 0` = failed. (path_id is the only length-returning one.)

- [ ] **Step 4: Re-express `run_builtins` as a loop** — replace its per-bit `if (sel & …)` body with the fixed-order dispatch through `run_builtin_bit`, preserving order HWDB→PATH→USB→INPUT→NET→V4L→ATA→BLKID→CDROM:

```c
static inline int run_builtins(const char *sysroot, const char *devpath,
                               const char *devnode, struct uevent *ev) {
    int before = ev->n;
    int sel = ub_select(sysroot, devpath, devnode, ev);
    static const int order[] = { UB_HWDB, UB_PATH, UB_USB, UB_INPUT, UB_NET,
                                 UB_V4L, UB_ATA, UB_BLKID, UB_CDROM };
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++)
        if (sel & order[i]) run_builtin_bit(sysroot, devpath, devnode, ev, order[i]);
    return ev->n - before;
}
```

- [ ] **Step 5: Run parity + existing builtins tests**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_builtins.c -o /tmp/schema-test-ub && /tmp/schema-test-ub && cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: both PASS (`test_udev_r4a: run_builtin_bit OK`), `run_builtins` behavior unchanged.

- [ ] **Step 6: Commit**

```bash
git add udev_builtins.h tests/test_udev_r4a.c
git commit -m "refactor(schema-udev): extract run_builtin_bit from run_builtins (status-returning)"
```

---

### Task 4: IMPORT dispatch + hard-gate + deferred accounting

**Files:**
- Modify: `udev_ruleset.h` (`apply_rule` IMPORT branch ~line 518; `ruleset_apply` ~line 535; `rule_match` for IMPORT{program} match-side is N/A — IMPORT is assign-op)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Consumes: `run_builtin_bit`, `UB_*` bits, `dev_ctx`.
- Produces: `int builtin_name_bit(const char *name)` → `UB_*` bit or `0` (un-ported); `int apply_import(struct dev_ctx *ctx, const struct rule_clause *c, const char *sv)` → `1` continue / `0` hard-gate-stop. `apply_rule` returns `NULL` early on hard-gate. `ruleset_apply` bumps `deferred_applies` **after** `apply_rule`.

- [ ] **Step 1: Write the failing test** — append to `main()`. This is the decision table's teeth (soft vs hard vs deferred). Uses `usb_id` as the ported builtin (fails on a bogus device → hard gate):

```c
    /* Task 4: IMPORT{builtin} gate semantics */
    {
        struct uevent gev; memset(&gev, 0, sizeof gev);
        ue_set(&gev, "ACTION", "add"); ue_set(&gev, "DEVPATH", "/devices/none");
        struct dev_ctx gc; assert(dev_ctx_init(&gc, &gev, "/nonexistent-sysroot") == 0);

        /* ported builtin that FAILS -> hard gate: later ENV assignment must NOT apply */
        struct rule r;
        ruleset_parse_line("IMPORT{builtin}=\"usb_id\", ENV{AFTER}=\"1\"", &r);
        assert(rule_match(&r, &gc) == 1);
        apply_rule(&r, &gc);
        assert(uevent_get(&gc.ev[0], "AFTER") == NULL || uevent_get(gc.ev, "AFTER") == NULL);

        /* un-ported builtin -> deferred, NOT a gate: later ENV assignment DOES apply */
        struct uevent dev2; memset(&dev2, 0, sizeof dev2);
        ue_set(&dev2, "ACTION", "add"); ue_set(&dev2, "DEVPATH", "/devices/none");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev2, "/sys") == 0);
        ruleset_parse_line("IMPORT{builtin}=\"keyboard\", ENV{AFTER}=\"1\"", &r);
        assert(rule_match(&r, &dc) == 1);
        dc.last_rule_deferred = 0;
        apply_rule(&r, &dc);
        assert(strcmp(uevent_get(dc.ev, "AFTER"), "1") == 0);
        assert(dc.last_rule_deferred == 1);

        /* IMPORT{program} -> deferred, NOT a gate */
        struct uevent pev; memset(&pev, 0, sizeof pev);
        ue_set(&pev, "ACTION", "add"); ue_set(&pev, "DEVPATH", "/devices/none");
        struct dev_ctx pc; assert(dev_ctx_init(&pc, &pev, "/sys") == 0);
        ruleset_parse_line("IMPORT{program}=\"/bin/true\", ENV{AFTER}=\"1\"", &r);
        pc.last_rule_deferred = 0;
        apply_rule(&r, &pc);
        assert(strcmp(uevent_get(pc.ev, "AFTER"), "1") == 0);
        assert(pc.last_rule_deferred == 1);

        /* deferred bump is counted once by ruleset_apply, after apply */
        struct uevent sev; memset(&sev, 0, sizeof sev);
        ue_set(&sev, "ACTION", "add"); ue_set(&sev, "DEVPATH", "/devices/none");
        struct dev_ctx sc2; assert(dev_ctx_init(&sc2, &sev, "/sys") == 0);
        struct ruleset rs; memset(&rs, 0, sizeof rs);
        ruleset_parse_line("IMPORT{builtin}=\"keyboard\"", &rs.rules[0]); rs.n = 1;
        ruleset_apply(&rs, &sc2);
        assert(sc2.deferred_applies == 1);
    }
    printf("test_udev_r4a: IMPORT-gate OK\n");
```

(Simplify the `AFTER == NULL` assert to just `assert(uevent_get(gc.ev, "AFTER") == NULL);` — the hard-gated rule never sets it.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: FAIL — IMPORT is currently a no-op; `AFTER` gets set on the hard-gate case, `deferred_applies` stays 0.

- [ ] **Step 3: Add `builtin_name_bit` and `apply_import`** — first add the two header includes near the top of `udev_ruleset.h` (after `path_id.h`; `<sys/stat.h>` was added in Task 2):

```c
#include "udev_db.h"        /* udev_db_filename, udev_db_read_eprops (IMPORT{db}/{parent}) */
#include "udev_builtins.h"  /* run_builtin_bit, UB_* (IMPORT{builtin}) */
```

Then, above `apply_rule`:

```c
static inline int builtin_name_bit(const char *name) {
    if (!strcmp(name, "hwdb"))     return UB_HWDB;
    if (!strcmp(name, "path_id"))  return UB_PATH;
    if (!strcmp(name, "usb_id"))   return UB_USB;
    if (!strcmp(name, "input_id")) return UB_INPUT;
    if (!strcmp(name, "net_id"))   return UB_NET;
    if (!strcmp(name, "blkid"))    return UB_BLKID;
    return 0;   /* keyboard, factory_reset, dissect_image, btrfs, net_setup_link, ... */
}

/* 1 = continue applying rule; 0 = hard gate (stop this rule). */
static inline int apply_import(struct dev_ctx *ctx, const struct rule_clause *c, const char *sv) {
    if (!strcmp(c->subkey, "cmdline")) { import_cmdline(ctx, c->val); return 1; }
    if (!strcmp(c->subkey, "db"))      { import_db(ctx, c->val);      return 1; }
    if (!strcmp(c->subkey, "parent"))  { import_parent(ctx, c->val);  return 1; }
    if (!strcmp(c->subkey, "builtin")) {
        int bit = builtin_name_bit(sv);
        if (bit == 0) { ctx->last_rule_deferred = 1; return 1; }   /* un-ported: defer, no gate */
        char devnode[PATH_MAX]; const char *dn = NULL;
        const char *name = uevent_get(ctx->ev, "DEVNAME");
        if (name && *name) { snprintf(devnode, sizeof devnode, "/dev/%s", name); dn = devnode; }
        const char *dp = uevent_get(ctx->ev, "DEVPATH");
        int rc = run_builtin_bit(ctx->sysroot, dp ? dp : "", dn, ctx->ev, bit);
        return (rc < 0) ? 0 : 1;   /* hard gate on builtin failure */
    }
    /* program / file / other: deferred to R4b */
    ctx->last_rule_deferred = 1;
    return 1;
}
```

`import_cmdline`/`import_db`/`import_parent` are defined in Tasks 5–7; add forward-usable stubs now so this compiles, or reorder so this task lands after them. **To keep TDD honest, implement Tasks 5–7's three functions as no-op stubs here** (`static inline void import_cmdline(struct dev_ctx *c, const char *k){ (void)c;(void)k; }` etc.), then flesh them out in their own tasks. Place the stubs directly above `apply_import`.

- [ ] **Step 4: Wire IMPORT/RUN into `apply_rule`** — replace the `/* IMPORT / RUN / other: R4 — ignored here */` comment with:

```c
        } else if (!strcmp(c->key, "IMPORT")) {
            if (apply_import(ctx, c, sv) == 0) return NULL;   /* hard gate: stop this rule */
        } else if (!strcmp(c->key, "RUN")) {
            ctx_add_run(ctx, sv);
```

Add `ctx_add_run` above `apply_rule`:

```c
static inline void ctx_add_run(struct dev_ctx *ctx, const char *v) {
    if (ctx->nruns >= DEVCTX_RUNS_MAX) return;
    safe_copy(ctx->runs[ctx->nruns++], v, UE_VAL_MAX);
}
```

Note: `sv` is the `ruleset_subst`-expanded value already computed at the top of the `apply_rule` loop body. For `IMPORT{builtin}` the builtin name is `sv` (subst is a no-op on a bare name); for `IMPORT{db}`/`{parent}` the key/glob is `c->val` (used raw inside the import fns), which is why `apply_import` takes both `c` and `sv`.

- [ ] **Step 5: Reorder deferred accounting in `ruleset_apply`** — change:

```c
        if (!rule_match(&rs->rules[i], ctx)) { i++; continue; }
        if (ctx->last_rule_deferred) ctx->deferred_applies++;
        const char *goto_label = apply_rule(&rs->rules[i], ctx);
```

to count **after** apply (so apply-side deferrals are included):

```c
        if (!rule_match(&rs->rules[i], ctx)) { i++; continue; }
        const char *goto_label = apply_rule(&rs->rules[i], ctx);
        if (ctx->last_rule_deferred) ctx->deferred_applies++;
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: IMPORT-gate OK`.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c
git commit -m "feat(schema-udev): R4a IMPORT dispatch, builtin hard-gate, deferred accounting reorder"
```

---

### Task 5: `import_cmdline`

**Files:**
- Modify: `udev_ruleset.h` (replace the `import_cmdline` stub from Task 4)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Produces: `void import_cmdline(struct dev_ctx *ctx, const char *key)` — reads `ctx->cmdline_path`, tokenizes on whitespace, sets `key` (or `key=val`) into `ctx->ev`.

- [ ] **Step 1: Write the failing test** — append to `main()`:

```c
    /* Task 5: IMPORT{cmdline} */
    {
        char cf[] = "/tmp/r4a_cmdlineXXXXXX";
        int fd = mkstemp(cf); assert(fd >= 0);
        dprintf(fd, "quiet root=/dev/sda2 rd.foo=bar\n"); close(fd);

        struct uevent cev; memset(&cev, 0, sizeof cev);
        ue_set(&cev, "ACTION", "add"); ue_set(&cev, "DEVPATH", "/devices/x");
        struct dev_ctx cc; assert(dev_ctx_init(&cc, &cev, "/sys") == 0);
        cc.cmdline_path = cf;

        import_cmdline(&cc, "rd.foo");
        assert(strcmp(uevent_get(cc.ev, "rd.foo"), "bar") == 0);
        import_cmdline(&cc, "quiet");
        assert(strcmp(uevent_get(cc.ev, "quiet"), "1") == 0);   /* bare flag -> "1" */
        import_cmdline(&cc, "absent");
        assert(uevent_get(cc.ev, "absent") == NULL);            /* soft: no-op */
        unlink(cf);
    }
    printf("test_udev_r4a: IMPORT-cmdline OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: FAIL — the stub imports nothing; `rd.foo` lookup returns NULL.

- [ ] **Step 3: Implement `import_cmdline`** — replace the stub:

```c
static inline void import_cmdline(struct dev_ctx *ctx, const char *key) {
    FILE *f = fopen(ctx->cmdline_path, "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t klen = strlen(key);
    for (char *p = buf; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t tlen = (size_t)(p - s);
        if (tlen == 0) continue;
        if (tlen >= klen && !strncmp(s, key, klen)) {
            if (tlen == klen) { uevent_set(ctx->ev, key, "1"); return; }
            if (s[klen] == '=') {
                char val[UE_VAL_MAX];
                size_t vlen = tlen - klen - 1;
                if (vlen >= sizeof val) vlen = sizeof val - 1;
                memcpy(val, s + klen + 1, vlen); val[vlen] = '\0';
                uevent_set(ctx->ev, key, val); return;
            }
        }
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: IMPORT-cmdline OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c
git commit -m "feat(schema-udev): R4a IMPORT{cmdline}"
```

---

### Task 6: `import_db`

**Files:**
- Modify: `udev_ruleset.h` (replace the `import_db` stub; ensure `#include "udev_db.h"` is present)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Consumes: `udev_db_filename`, `udev_db_read_eprops`.
- Produces: `void import_db(struct dev_ctx *ctx, const char *key)` — locates `ctx->dbroot/<db-filename-of-this-device>`, reads its `E:` props, imports the single named `key` into `ctx->ev`.

- [ ] **Step 1: Write the failing test** — append to `main()`. The device is a block dev `b8:0`, so `udev_db_filename` yields `b8:0`:

```c
    /* Task 6: IMPORT{db} */
    {
        char dbdir[] = "/tmp/r4a_dbXXXXXX"; assert(mkdtemp(dbdir));
        char rec[PATH_MAX]; snprintf(rec, sizeof rec, "%s/b8:0", dbdir);
        FILE *f = fopen(rec, "w"); assert(f);
        fprintf(f, "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=dead-beef\nS:disk/by-uuid/dead-beef\n");
        fclose(f);

        struct uevent dev; memset(&dev, 0, sizeof dev);
        ue_set(&dev, "ACTION", "add"); ue_set(&dev, "DEVPATH", "/devices/virtual/block/sda");
        ue_set(&dev, "SUBSYSTEM", "block"); ue_set(&dev, "MAJOR", "8"); ue_set(&dev, "MINOR", "0");
        ue_set(&dev, "DEVNAME", "sda");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev, "/sys") == 0);
        dc.dbroot = dbdir;

        import_db(&dc, "ID_FS_TYPE");
        assert(strcmp(uevent_get(dc.ev, "ID_FS_TYPE"), "ext4") == 0);
        import_db(&dc, "ID_FS_UUID");
        assert(strcmp(uevent_get(dc.ev, "ID_FS_UUID"), "dead-beef") == 0);
        import_db(&dc, "NOPE");
        assert(uevent_get(dc.ev, "NOPE") == NULL);       /* missing key: no-op */
        unlink(rec); rmdir(dbdir);

        /* missing file: no-op, no crash */
        dc.dbroot = "/tmp/r4a_absent_db";
        import_db(&dc, "ID_FS_TYPE");                    /* still ext4 from before, unchanged */
    }
    printf("test_udev_r4a: IMPORT-db OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: FAIL — stub imports nothing.

- [ ] **Step 3: Implement `import_db`** — (`udev_db.h` was included in Task 4.) Replace the stub:

```c
static inline void import_db(struct dev_ctx *ctx, const char *key) {
    char fn[128];
    if (udev_db_filename(ctx->ev, fn, sizeof fn) != 0) return;
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->dbroot, fn) >= sizeof path) return;
    struct uevent rec;
    if (udev_db_read_eprops(path, &rec) != 0) return;
    const char *v = uevent_get(&rec, key);
    if (v) uevent_set(ctx->ev, key, v);
}
```

Confirm `udev_db_filename`'s signature and return contract (`0` on success) in `udev_db.h`; adapt if it returns the length or a pointer instead.

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: IMPORT-db OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c
git commit -m "feat(schema-udev): R4a IMPORT{db} via udev_db reader"
```

---

### Task 7: `import_parent`

**Files:**
- Modify: `udev_ruleset.h` (replace the `import_parent` stub)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Consumes: `pi_parent`, `uevent_from_sysfs`, `udev_db_filename`, `udev_db_read_eprops`, `udev_glob`.
- Produces: `void import_parent(struct dev_ctx *ctx, const char *keypat)` — resolves the parent sysfs dir, synthesizes the parent uevent, reads the parent's db record, imports every prop whose key matches `keypat` (glob) into `ctx->ev`.

- [ ] **Step 1: Write the failing test** — append to `main()`. Build a fake sysfs child+parent and a parent db record keyed by the parent's `+subsystem:sysname` (block subdev has no major/minor as a `+`-form; to keep it simple use a parent that is a block dev with major/minor so `udev_db_filename` produces `b<maj>:<min>`):

```c
    /* Task 7: IMPORT{parent} */
    {
        char root[] = "/tmp/r4a_sysXXXXXX"; assert(mkdtemp(root));
        /* child: <root>/devices/pci/blk/sda1 ; parent: .../blk (block, b8:0) */
        char parent[PATH_MAX], child[PATH_MAX];
        snprintf(parent, sizeof parent, "%s/devices/pci/blk", root);
        snprintf(child,  sizeof child,  "%s/devices/pci/blk/sda1", root);
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", child); assert(system(cmd) == 0);
        /* parent uevent gives MAJOR/MINOR so its db filename is b8:0 */
        char uev[PATH_MAX]; snprintf(uev, sizeof uev, "%s/uevent", parent);
        FILE *f = fopen(uev, "w"); assert(f);
        fprintf(f, "MAJOR=8\nMINOR=0\nDEVNAME=sda\nSUBSYSTEM=block\n"); fclose(f);

        char dbdir[] = "/tmp/r4a_pdbXXXXXX"; assert(mkdtemp(dbdir));
        char rec[PATH_MAX]; snprintf(rec, sizeof rec, "%s/b8:0", dbdir);
        f = fopen(rec, "w"); assert(f);
        fprintf(f, "E:ID_SERIAL=WDC-123\nE:ID_MODEL=WDC\nE:OTHER=x\n"); fclose(f);

        struct uevent dev; memset(&dev, 0, sizeof dev);
        ue_set(&dev, "ACTION", "add");
        ue_set(&dev, "DEVPATH", "/devices/pci/blk/sda1");
        ue_set(&dev, "SUBSYSTEM", "block");
        struct dev_ctx dc; assert(dev_ctx_init(&dc, &dev, root) == 0);
        dc.dbroot = dbdir;

        import_parent(&dc, "ID_*");
        assert(strcmp(uevent_get(dc.ev, "ID_SERIAL"), "WDC-123") == 0);
        assert(strcmp(uevent_get(dc.ev, "ID_MODEL"), "WDC") == 0);
        assert(uevent_get(dc.ev, "OTHER") == NULL);     /* glob did not match */

        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root, dbdir); assert(system(cmd) == 0);
    }
    printf("test_udev_r4a: IMPORT-parent OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: FAIL — stub imports nothing.

- [ ] **Step 3: Implement `import_parent`** — replace the stub. `pi_parent` mutates a path buffer in place (climb one level); `uevent_from_sysfs` synthesizes the parent's uevent so `udev_db_filename` resolves its record name:

```c
static inline void import_parent(struct dev_ctx *ctx, const char *keypat) {
    char pdir[PATH_MAX];
    safe_copy(pdir, ctx->sysdir, sizeof pdir);
    if (pi_parent(pdir) != 0) return;                 /* no parent */
    struct uevent pev;
    if (uevent_from_sysfs(ctx->sysroot, pdir, &pev) != 0) return;
    char fn[128];
    if (udev_db_filename(&pev, fn, sizeof fn) != 0) return;
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", ctx->dbroot, fn) >= sizeof path) return;
    struct uevent rec;
    if (udev_db_read_eprops(path, &rec) != 0) return;
    for (int i = 0; i < rec.n; i++)
        if (udev_glob(keypat, rec.key[i]))
            uevent_set(ctx->ev, rec.key[i], rec.val[i]);
}
```

Confirm `uevent_from_sysfs`'s signature (`sysroot, dirpath, ev`) and that it derives DEVPATH from `dirpath` relative to `sysroot`; if it needs the DEVPATH pre-set, adapt. Confirm `pi_parent` returns `0` on success.

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: IMPORT-parent OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4a.c
git commit -m "feat(schema-udev): R4a IMPORT{parent} (parent db inherit, glob keys)"
```

---

### Task 8: RUN record (execute nothing)

**Files:**
- Modify: `udev_ruleset.h` (`ctx_add_run` already added in Task 4 — this task verifies the end-to-end record + no-exec)
- Test: `tests/test_udev_r4a.c`

**Interfaces:**
- Consumes: `ctx_add_run`, `apply_rule`.
- Produces: (no new symbol) — a verified invariant that `RUN+=` populates `ctx->runs` and executes nothing.

- [ ] **Step 1: Write the failing test** — append to `main()`. The RUN command *would* create a marker file; assert it does not:

```c
    /* Task 8: RUN records intent, executes nothing */
    {
        char marker[] = "/tmp/r4a_marker_shouldnotexist";
        unlink(marker);
        struct uevent rev; memset(&rev, 0, sizeof rev);
        ue_set(&rev, "ACTION", "add"); ue_set(&rev, "DEVPATH", "/devices/x");
        struct dev_ctx rc; assert(dev_ctx_init(&rc, &rev, "/sys") == 0);
        struct rule r;
        char line[256];
        snprintf(line, sizeof line, "RUN+=\"/bin/touch %s\"", marker);
        ruleset_parse_line(line, &r);
        apply_rule(&r, &rc);
        assert(rc.nruns == 1);
        assert(strstr(rc.runs[0], "/bin/touch") != NULL);
        assert(access(marker, F_OK) != 0);    /* NOT executed */
    }
    printf("test_udev_r4a: RUN-record OK\n");
```

- [ ] **Step 2: Run test to verify it passes** (Task 4 already wired `RUN` → `ctx_add_run`)

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_r4a.c -o /tmp/schema-test-r4a && /tmp/schema-test-r4a`
Expected: PASS — `test_udev_r4a: RUN-record OK`. (If `RUN{builtin}` needs the same path, note its clause key is also `"RUN"` with subkey `"builtin"` — `ctx_add_run` records the value regardless; that is correct, still no exec.)

- [ ] **Step 3: Commit** (only if the test file changed since Task 4)

```bash
git add tests/test_udev_r4a.c
git commit -m "test(schema-udev): R4a RUN records intent, executes nothing"
```

---

### Task 9: Full-suite gates + c11 + live-smoke + boundary

**Files:**
- Modify: none (verification task); may touch `udev_ruleset.h`/`udev_builtins.h` only to fix warnings.
- Test: whole `make test`, explicit c11 build, live-smoke.

**Interfaces:** none.

- [ ] **Step 1: Run the whole suite (c99)**

Run: `cd /home/ajax80/projects/schema-init && make test 2>&1 | tail -40`
Expected: every `test_*` line runs, exit 0, no failures.

- [ ] **Step 2: c11 gate on the new + touched TUs (zero warnings)**

Run:
```bash
for t in tests/test_udev_r4a.c tests/test_udev_builtins.c tests/test_udev_executor.c tests/test_udev_matcher.c; do
  cc -std=c11 -Wall -Wextra -D_GNU_SOURCE -I. "$t" -o /tmp/c11check && /tmp/c11check || { echo "FAIL $t"; break; }
done
```
Expected: all compile with **zero warnings** and pass. Fix any c11-only diagnostic in the headers.

- [ ] **Step 3: Live-smoke `ruleset_apply` on real `/sys/block/sda`** — write `/tmp/r4a_smoke.c` that loads the installed rules, builds a `dev_ctx` for a real device, and runs `ruleset_apply`, printing tags/nruns/deferred_applies:

```c
#include "udev_ruleset.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    struct ruleset rs; memset(&rs, 0, sizeof rs);
    const char *dirs[] = { "/etc/udev/rules.d", "/run/udev/rules.d", "/usr/lib/udev/rules.d" };
    ruleset_load_dirs(dirs, 3, &rs);   /* precedence order: etc > run > usr/lib */
    struct uevent ev; memset(&ev, 0, sizeof ev);
    safe_copy(ev.key[ev.n], "ACTION", UE_KEY_MAX); safe_copy(ev.val[ev.n++], "add", UE_VAL_MAX);
    safe_copy(ev.key[ev.n], "DEVPATH", UE_KEY_MAX); safe_copy(ev.val[ev.n++], "/devices/…/block/sda", UE_VAL_MAX); /* real path from: udevadm info -q path -n /dev/sda */
    safe_copy(ev.key[ev.n], "SUBSYSTEM", UE_KEY_MAX); safe_copy(ev.val[ev.n++], "block", UE_VAL_MAX);
    struct dev_ctx ctx; if (dev_ctx_init(&ctx, &ev, "/sys") != 0) return 1;
    ruleset_apply(&rs, &ctx);
    printf("smoke: rules=%d ntags=%d nruns=%d deferred=%d\n", rs.n, ctx.ntags, ctx.nruns, ctx.deferred_applies);
    return 0;
}
```

Run:
```bash
SDA=$(udevadm info -q path -n /dev/sda)   # fill the real DEVPATH into the source
cc -std=c11 -Wall -Wextra -D_GNU_SOURCE -I. /tmp/r4a_smoke.c -o /tmp/r4a_smoke && /tmp/r4a_smoke
```
Expected: no crash; prints a plausible `rules=…` (hundreds), `deferred=` a small residual (PROGRAM/RESULT/IMPORT{program} rules that match sda). **Confirm the live box is untouched** afterward: `sudo udevadm info /dev/sda` unchanged; no writes under `/run/udev/data` from us (we only read).

- [ ] **Step 4: Boundary check — `schema-udev.c` byte-identical**

Run: `git diff HEAD -- schema-udev.c` (empty) and `git diff --stat HEAD~8..HEAD` to confirm only `udev_ruleset.h`, `udev_builtins.h`, `tests/test_udev_r4a.c`, `Makefile`, and the docs changed.
Expected: `schema-udev.c` absent from the diff.

- [ ] **Step 5: Final commit (if any warning-fixes were needed)**

```bash
git add -u
git commit -m "chore(schema-udev): R4a c11 clean + live-smoke gate"
```

---

## Self-Review

**Spec coverage:**
- TEST (stat + octal) → Task 2. ✓
- IMPORT{cmdline} → Task 5. ✓
- IMPORT{db} → Task 6. ✓
- IMPORT{parent} → Task 7. ✓
- IMPORT{builtin} reuse of udev_builtins.h via run_builtin_bit → Tasks 3 + 4. ✓
- Soft/hard/deferred decision table → Task 4 (test is its teeth). ✓
- apply_rule early-return gate mechanism → Task 4. ✓
- deferred_applies accounting reorder → Task 4. ✓
- RUN record, execute nothing → Tasks 4 (wire) + 8 (verify). ✓
- dev_ctx growth (runs/dbroot/cmdline_path), DEVCTX_RUNS_MAX justified → Task 1. ✓
- Superset shrink / re-gate → Task 2 (re-gate assert) + Task 4 (deferred count). ✓
- Gates: make test + c11 + live-smoke + schema-udev.c boundary → Task 9. ✓
- Out-of-scope (IMPORT{program}, PROGRAM/RESULT, un-ported builtins, RUN exec) → deferred, un-ported builtin path asserts "not a gate". ✓

**Placeholder scan:** No TBD/TODO. Three "confirm the signature" notes (udev_db_filename, uevent_from_sysfs, pi_parent, ruleset loader name) are deliberate verification steps against the real headers, each with the expected contract stated and an adaptation instruction — not deferred work.

**Type consistency:** `run_builtin_bit(sysroot,devpath,devnode,ev,bit)→int` used identically in Tasks 3/4. `apply_import(ctx,c,sv)→int` (1 continue/0 gate) and `builtin_name_bit(name)→int` consistent. `import_cmdline(ctx,key)` / `import_db(ctx,key)` / `import_parent(ctx,keypat)` all `void`, stubbed in Task 4, implemented in 5/6/7 with matching signatures. `ctx_add_run(ctx,v)→void` defined Task 4, used Task 8. `dev_ctx` fields `dbroot`/`cmdline_path`/`runs`/`nruns` defined Task 1, used throughout.

## Resolved before execution (confirmed against source)

- Loader is `ruleset_load_dirs(const char *const *dirs, int ndirs, struct ruleset *rs)` — used correctly in the Task 9 smoke.
- `udev_db_filename` returns `0` on success and writes a bare basename (`b8:0` / `c…` / `n…` / `+subsys:sysname`); reader path is `dbroot + "/" + fn`. ✓
- `udev_ruleset.h` includes **neither** `udev_db.h` nor `udev_builtins.h` today → Task 2 adds `<sys/stat.h>`, Task 4 adds both header includes. ✓
- `uevent_from_sysfs(sysroot, dirpath, ev)` reads the device's `uevent` file (MAJOR/MINOR/SUBSYSTEM) → parent db filename resolves in Task 7. ✓

## Still confirm during execution (per-port, low-risk)

- Each `*_build`'s exact return convention when moving bodies into `run_builtin_bit` (Task 3 Step 3): `0` = ran, `< 0` = failed; `path_id_build` is length-returning (`> 0` = ok). Adapt the mapping per header if any differs.
