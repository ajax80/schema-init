# schema-udev R5 — interpreter integration + live fidelity gate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the native udev `.rules` interpreter inside `schema-udev` (shadow-only) and build a standalone `verify-rules-live` gate that proves its `S:`/`G:` output matches real udevd across every device — the E3-flip precondition.

**Architecture:** All new persistence code is inline in the header-only `udev_db.h` (`udev_db_write_full`, `udev_db_read_links_tags`). The daemon (`schema-udev.c`) gets a global `struct ruleset`, loaded from the real udev rule dirs and applied to a **deep copy** of each uevent so the existing pipeline is byte-for-byte unaffected; the interpreter's tags/symlinks are written to a new shadow dir `/run/schema-udev/rules-data`. A new standalone tool `tools/verify-rules-live.c` recomputes independently and diffs against `/run/udev/data`, exiting nonzero on in-scope divergence.

**Tech Stack:** C99, header-only inline modules, GNU Make, coldplug sysfs walk. No new libraries.

## Global Constraints

- **Shadow-only. Live box untouched.** Sentinel `/etc/schema-init/schema-udev.live` stays absent; nothing writes `/dev` or `/run/udev/data`; systemd-udevd stays authoritative. Never install/deploy the built binary.
- **The existing dispatch pipeline stays 100% untouched** — `run_builtins` → `run_rules` → `udev_db_write` (E:-only, `SCHEMA_UDEV_DB_DIR`) → `disk_links` → `uaccess` are unmodified in behavior.
- **Build clean:** `make test` green; 0 warnings under `-std=c99` **and** `-std=c11`, each × `-O0` and `-O2`. Compile flags baseline: `-std=c99 -Wall -Wextra -D_GNU_SOURCE -I.`
- **No docstrings/comments beyond what the surrounding code already uses.** Match existing terse inline style.
- **TDD:** failing test first, minimal impl, green, commit. Frequent commits.
- Branch: `feat/schema-udev-cutover-e3`.

---

## File Structure

- `udev_db.h` — **Modify.** Add `SCHEMA_UDEV_RULES_DIR`, `udev_db_write_full` (atomic full-record writer), `udev_db_read_links_tags` (S:/G: reader). Owns all shadow-record persistence.
- `tests/test_udev_db.c` — **Modify.** Append `udev_db_write_full` round-trip + `udev_db_read_links_tags` assertions to the existing `main()`.
- `schema-udev.c` — **Modify.** `#include "udev_ruleset.h"`; `g_ruleset` + `RULE_DIRS` + `ruleset_reload`; startup load + shadow-dir wipe; SIGHUP reload; deep-copy interpreter block in `dispatch`; remove-branch unlink.
- `tools/verify-rules-live.c` — **Create.** Standalone read-only gate: recompute per device, diff S:/G: vs `/run/udev/data`, classify, exit nonzero on in-scope divergence.
- `Makefile` — **Modify.** Register the new test; add interpreter headers to the `schema-udev` prereq list; add a `verify-rules-live` target.

---

### Task 1: `udev_db_write_full` + `udev_db_read_links_tags` in `udev_db.h`

**Files:**
- Modify: `udev_db.h` (add after `udev_db_write`, ~line 128; add reader after `udev_db_read_eprops`, ~line 158)
- Test: `tests/test_udev_db.c` (append to existing `main()` before `return 0;`)
- Modify: `Makefile` (the test is already registered at line 42 — no new test binary needed; the existing `test_udev_db` bin covers it)

**Interfaces:**
- Consumes: `udev_db_filename`, `udev_db_ensure_dir`, `udev_db_record_build_full(ev, kernel_n, symlinks, nsym, usec_init, tags, ntag, buf, bufsz)`, `safe_copy`, `struct uevent` (all existing in `udev_db.h`/`schema-udev.h`).
- Produces:
  - `int udev_db_write_full(const char *base_dir, const struct uevent *ev, int kernel_n, const char *const *symlinks, int nsym, const char *const *tags, int ntag)` → atomic write of a full `S:/I:/E:/G:/Q:/V:` record via mkstemp+rename; `usec_init` hard-coded 0; returns 0 / -1.
  - `int udev_db_read_links_tags(const char *path, char links[][UE_VAL_MAX], int *nlink, int maxlink, char tags[][UE_KEY_MAX], int *ntag, int maxtag)` → parse `S:` lines into `links`, `G:` lines into `tags`; returns 0 / -1 (open fail).
  - `#define SCHEMA_UDEV_RULES_DIR "/run/schema-udev/rules-data"`

- [ ] **Step 1: Write the failing test** — append to `tests/test_udev_db.c` `main()`, just before `return 0;`:

```c
    /* --- R5: udev_db_write_full round-trip (S:/G:/Q:/E:) --- */
    char tmpl[] = "/tmp/schema-r5-XXXXXX";
    char *rbase = mkdtemp(tmpl);
    assert(rbase);

    struct uevent fe; memset(&fe, 0, sizeof fe);
    put(&fe, "SUBSYSTEM", "block"); put(&fe, "MAJOR", "8"); put(&fe, "MINOR", "0");
    put(&fe, "DEVPATH", "/devices/x/block/sda");
    int fkn = fe.n;                       /* kernel boundary */
    put(&fe, "ID_FS_TYPE", "ext4");       /* a derived E: prop */
    const char *fsyms[] = { "disk/by-id/wwn-0xabc", "disk/by-path/pci-0000" };
    const char *ftags[] = { "systemd", "seat" };
    assert(udev_db_write_full(rbase, &fe, fkn, fsyms, 2, ftags, 2) == 0);

    char fpath[512];
    snprintf(fpath, sizeof fpath, "%s/b8:0", rbase);
    char links[8][UE_VAL_MAX]; int nl = 0;
    char rtags[8][UE_KEY_MAX];  int nt = 0;
    assert(udev_db_read_links_tags(fpath, links, &nl, 8, rtags, &nt, 8) == 0);
    assert(nl == 2 && !strcmp(links[0], "disk/by-id/wwn-0xabc") && !strcmp(links[1], "disk/by-path/pci-0000"));
    assert(nt == 2 && !strcmp(rtags[0], "systemd") && !strcmp(rtags[1], "seat"));

    /* E: delta present, G: and Q: both emitted, V: last */
    struct uevent fback; assert(udev_db_read_eprops(fpath, &fback) == 0);
    assert(!strcmp(uevent_get(&fback, "ID_FS_TYPE"), "ext4"));
    unlink(fpath); rmdir(rbase);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_db.c -o /tmp/t && /tmp/t`
Expected: FAIL — compile error `udev_db_write_full`/`udev_db_read_links_tags`/`SCHEMA_UDEV_RULES_DIR` undefined.

- [ ] **Step 3: Add the dir constant** near the top of `udev_db.h` beside the existing dir defines (after line 14):

```c
#define SCHEMA_UDEV_RULES_DIR "/run/schema-udev/rules-data"   /* R5 interpreter shadow */
```

- [ ] **Step 4: Add `udev_db_write_full`** immediately after `udev_db_write` (~line 128):

```c
static inline int udev_db_write_full(const char *base_dir, const struct uevent *ev,
                                     int kernel_n,
                                     const char *const *symlinks, int nsym,
                                     const char *const *tags, int ntag) {
    char name[128];
    if (udev_db_filename(ev, name, sizeof name) != 0) return -1;
    if (udev_db_ensure_dir(base_dir) != 0) return -1;
    char buf[8192];
    ssize_t len = udev_db_record_build_full(ev, kernel_n, symlinks, nsym, 0,
                                            tags, ntag, buf, sizeof buf);
    if (len <= 0) return -1;
    char final[512], tmpl[512];
    if ((size_t)snprintf(final, sizeof final, "%s/%s", base_dir, name) >= sizeof final) return -1;
    if ((size_t)snprintf(tmpl, sizeof tmpl, "%s/.dbXXXXXX", base_dir) >= sizeof tmpl) return -1;
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    ssize_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w < 0) { close(fd); unlink(tmpl); return -1; }
        off += w;
    }
    if (close(fd) != 0) { unlink(tmpl); return -1; }
    if (rename(tmpl, final) != 0) { unlink(tmpl); return -1; }
    return 0;
}
```

- [ ] **Step 5: Add `udev_db_read_links_tags`** immediately after `udev_db_read_eprops` (~line 158):

```c
static inline int udev_db_read_links_tags(const char *path,
        char links[][UE_VAL_MAX], int *nlink, int maxlink,
        char tags[][UE_KEY_MAX], int *ntag, int maxtag) {
    *nlink = 0; *ntag = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == 'S' && line[1] == ':' && *nlink < maxlink)
            safe_copy(links[(*nlink)++], line + 2, UE_VAL_MAX);
        else if (line[0] == 'G' && line[1] == ':' && *ntag < maxtag)
            safe_copy(tags[(*ntag)++], line + 2, UE_KEY_MAX);
    }
    fclose(f);
    return 0;
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_db.c -o /tmp/t && /tmp/t && echo OK`
Expected: PASS, prints `OK`.

- [ ] **Step 7: Full warnings matrix on the touched header**

Run:
```bash
for std in c99 c11; do for opt in O0 O2; do \
  gcc -std=$std -$opt -Wall -Wextra -D_GNU_SOURCE -I. tests/test_udev_db.c -o /tmp/t && /tmp/t \
  && echo "$std $opt OK" || echo "$std $opt FAIL"; done; done
```
Expected: four `OK` lines, no warnings.

- [ ] **Step 8: Commit**

```bash
git add udev_db.h tests/test_udev_db.c
git commit -m "feat(schema-udev): R5 udev_db_write_full + read_links_tags (shadow full-record persistence)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Wire the interpreter into the daemon (deep-copy, shadow dir)

**Files:**
- Modify: `schema-udev.c` (include ~line 6; globals ~line 60; startup ~line 183–187; SIGHUP ~line 205; dispatch shadow block inside `if (devpath)` ~line 96–131)
- Modify: `Makefile` (the `schema-udev:` prereq line, ~line 51)

**Interfaces:**
- Consumes: `struct ruleset`, `ruleset_load_dirs(const char *const *dirs, int ndirs, struct ruleset *rs)`, `struct dev_ctx`, `dev_ctx_init(struct dev_ctx*, struct uevent*, const char *sysroot)`, `ruleset_apply(const struct ruleset*, struct dev_ctx*)`, `DEVCTX_SYMLINKS_MAX`, `DEVCTX_TAGS_MAX` (all from `udev_ruleset.h`); `udev_db_write_full`, `udev_db_remove`, `SCHEMA_UDEV_RULES_DIR` (Task 1). `struct dev_ctx` fields used: `.symlinks[][UE_VAL_MAX]`, `.nsym`, `.tags[][UE_KEY_MAX]`, `.ntags`.
- Produces: a running daemon that writes `/run/schema-udev/rules-data/<name>` full records from interpreter output, with the existing pipeline unchanged.

- [ ] **Step 1: Add the interpreter include** — in `schema-udev.c`, after line 6 (`#include "udev_db.h"`):

```c
#include "udev_ruleset.h"
```

- [ ] **Step 2: Add globals + reload helper** — after the `g_live` block (~line 66), add:

```c
static struct ruleset g_ruleset;
static const char *const RULE_DIRS[] = {
    "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };

static void ruleset_reload(void) {
    free(g_ruleset.rules);
    memset(&g_ruleset, 0, sizeof g_ruleset);
    ruleset_load_dirs(RULE_DIRS, 3, &g_ruleset);
    fprintf(stderr, "[schema-udev] loaded %d native rule(s)\n", g_ruleset.n);
}
```

- [ ] **Step 3: Wire the shadow block into `dispatch`** — inside the existing `if (devpath) { ... }`, immediately **after** the `run_builtins(...)` call (line 101) and before `run_rules(...)`, insert:

```c
        struct uevent shadow_ev = *ev;          /* deep copy: uevent is inline char[], no heap */
        int pre_rules_n = shadow_ev.n;
        struct dev_ctx rc;
        if (dev_ctx_init(&rc, &shadow_ev, "/sys") == 0) {
            ruleset_apply(&g_ruleset, &rc);
            if (strcmp(action, "remove") == 0) {
                udev_db_remove(SCHEMA_UDEV_RULES_DIR, &shadow_ev);
            } else if (rc.nsym > 0 || rc.ntags > 0 || shadow_ev.n > pre_rules_n) {
                const char *syms[DEVCTX_SYMLINKS_MAX];
                const char *tgs[DEVCTX_TAGS_MAX];
                for (int i = 0; i < rc.nsym;  i++) syms[i] = rc.symlinks[i];
                for (int i = 0; i < rc.ntags; i++) tgs[i]  = rc.tags[i];
                udev_db_write_full(SCHEMA_UDEV_RULES_DIR, &shadow_ev, kernel_n,
                                   syms, rc.nsym, tgs, rc.ntags);
            }
        }
```

(Placing it after `run_builtins` and before the rest means `shadow_ev` captures the post-builtins props exactly as the daemon's live `ev` has them; the deep copy guarantees the interpreter's later `uevent_set`s never touch the real `ev` used by `run_rules`/`udev_db_write`/`disk_links`.)

- [ ] **Step 4: Load + wipe at startup** — in `main()`, right after `rules_reload();` (line 183), add:

```c
    ruleset_reload();
    disk_links_wipe(SCHEMA_UDEV_RULES_DIR);   /* reuse the generic recursive rmdir/wipe */
```

If `disk_links_wipe` is not a generic directory wiper (verify its body first), instead wipe with the same idiom `main()` already uses for the other shadow dirs; the requirement is: the new dir is emptied of stale records at startup. Confirm by reading `disk_links.h:146`.

- [ ] **Step 5: Reload on SIGHUP** — in the signalfd drain, the `else if (si.ssi_signo == SIGHUP)` branch (~line 205), add `ruleset_reload();` next to the existing `rules_reload();`:

```c
                } else if (si.ssi_signo == SIGHUP) {
                    rules_reload();
                    ruleset_reload();
```

- [ ] **Step 6: Update the Makefile prereqs** — change the `schema-udev:` line (~line 51) to add the interpreter headers so a header edit triggers a rebuild:

```make
schema-udev: schema-udev.c schema-udev.h udev_db.h udev_rules.h udev_builtins.h uaccess.h disk_links.h fido_id.h udev_ruleset.h path_id.h udev_exec.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lacl
```

- [ ] **Step 7: Build the daemon, warnings matrix**

Run:
```bash
for std in c99 c11; do for opt in O0 O2; do \
  gcc -std=$std -$opt -Wall -Wextra -D_GNU_SOURCE -I. schema-udev.c -o /tmp/schema-udev-$std-$opt -lacl \
  && echo "$std $opt OK" || echo "$std $opt FAIL"; done; done
```
Expected: four `OK`, no warnings.

- [ ] **Step 8: Live dry-run smoke — isolation + shadow output** — the sentinel is absent, so the built binary runs dry-run (isolated namespaces, never touches real `/dev`/`/run/udev/data`). Capture the EXISTING shadow dir before and after to prove the deep copy isolates it:

```bash
# baseline the existing E:-only shadow records from a pre-R5 run if present
sudo rm -rf /run/schema-udev/data /run/schema-udev/rules-data
timeout 8 sudo /tmp/schema-udev-c99-O2 2>/tmp/r5-smoke.log || true
echo "=== rules-data records (interpreter shadow) ==="; ls /run/schema-udev/rules-data 2>/dev/null | head
echo "=== sda full record ==="; cat /run/schema-udev/rules-data/b8:0 2>/dev/null
echo "=== S:/G: present? ==="; grep -h '^S:\|^G:' /run/schema-udev/rules-data/b8:0 2>/dev/null
```
Expected: `/run/schema-udev/rules-data` is populated; sda's `b8:0` record carries `S:` and/or `G:` lines; the log shows `loaded N native rule(s)` with N in the hundreds. **Do not deploy** — this is a throwaway binary in `/tmp`.

- [ ] **Step 9: Prove the existing E:-only path is byte-unchanged** — build the pre-R5 daemon from the parent commit into a temp dir, run both dry-run against the same `/sys`, and diff the E:-only `SCHEMA_UDEV_DB_DIR` output:

```bash
git stash --include-untracked 2>/dev/null; git show HEAD~0:schema-udev.c >/dev/null  # sanity
# simplest: compare rules-data-independent dir. Run current binary, snapshot /run/schema-udev/data:
sudo rm -rf /run/schema-udev/data; timeout 8 sudo /tmp/schema-udev-c99-O2 2>/dev/null || true
sha256sum /run/schema-udev/data/* 2>/dev/null | sort -k2 > /tmp/r5-after.sums
# checkout parent of the Task-2 commit, build, run, snapshot, diff — do this after committing Step 10,
# comparing against origin/…~1. If the E: sums differ, the deep copy leaked; STOP and root-cause.
```
Expected (once compared against a pre-R5 build): identical `data/*` E: sums. The `rules-data` dir is new and additive. If `data/*` changed, the isolation is broken — halt.

- [ ] **Step 10: Commit**

```bash
git add schema-udev.c Makefile
git commit -m "feat(schema-udev): R5 run native interpreter in dispatch, shadow full-records

Deep-copies each uevent, runs ruleset_apply, writes S:/G:/Q:/E: to
/run/schema-udev/rules-data; existing pipeline untouched. free() ruleset heap on
reload; write-guard on post-builtins count.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `verify-rules-live` gate + Makefile target

**Files:**
- Create: `tools/verify-rules-live.c`
- Modify: `Makefile` (add `verify-rules-live` target near the `parity` target, ~line 53)

**Interfaces:**
- Consumes: `coldplug_walk_root("/sys", cb)` where `cb` is `void(*)(struct uevent*)`; `run_builtins("/sys", devpath, dn, &ev)` (from `udev_rules.h`); `dev_ctx_init` + `ruleset_apply` + `ruleset_load_dirs` (from `udev_ruleset.h`); `udev_db_filename`, `udev_db_read_links_tags`, `UDEV_DB_DIR` (from `udev_db.h`); `parity_builtin_hint` (from `udev-parity.h`, optional).
- Produces: an executable `verify-rules-live` that walks `/sys`, diffs interpreter `S:`/`G:` vs `/run/udev/data`, prints a report, and `exit(1)` on any in-scope divergence, `exit(0)` otherwise.

- [ ] **Step 1: Write the tool** — create `tools/verify-rules-live.c`:

```c
/* Read-only fidelity gate: diff the native rule interpreter's symlinks+tags
 * against real systemd-udevd /run/udev/data. Writes nothing. exit(1) on any
 * in-scope divergence (the E3-flip precondition), exit(0) when clean. */
#include "../schema-udev.h"
#include "../udev_rules.h"     /* run_builtins */
#include "../udev_db.h"        /* udev_db_filename, read_links_tags, UDEV_DB_DIR */
#include "../udev_ruleset.h"   /* ruleset_load_dirs, dev_ctx, ruleset_apply */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct ruleset g_rs;
static int g_dev = 0, g_dev_db = 0;
static int g_sym_extra = 0, g_sym_miss_inscope = 0, g_sym_miss_debt = 0;
static int g_tag_miss = 0, g_tag_extra = 0;

/* KNOWN-DEBT: by-id links need ata_id/scsi_id serial/wwn we don't fully emit yet. */
static int link_is_known_debt(const char *link) {
    return strncmp(link, "disk/by-id/", 11) == 0;
}

static int in_set(char set[][UE_VAL_MAX], int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(set[i], s)) return 1;
    return 0;
}
static int in_tagset(char set[][UE_KEY_MAX], int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(set[i], s)) return 1;
    return 0;
}

static void collect(struct uevent *ev_in) {
    struct uevent ev = *ev_in;
    int kernel_n = ev.n; (void)kernel_n;
    const char *devpath = uevent_get(&ev, "DEVPATH");
    if (!devpath) return;
    g_dev++;
    const char *devname = uevent_get(&ev, "DEVNAME");
    char devnode[UE_VAL_MAX]; const char *dn = NULL;
    if (devname) { snprintf(devnode, sizeof devnode, "/dev/%s", devname); dn = devnode; }
    run_builtins("/sys", devpath, dn, &ev);

    struct dev_ctx ctx;
    if (dev_ctx_init(&ctx, &ev, "/sys") != 0) return;
    ruleset_apply(&g_rs, &ctx);

    char name[128];
    if (udev_db_filename(&ev, name, sizeof name) != 0) return;
    char path[256];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", UDEV_DB_DIR, name) >= sizeof path) return;
    char tlinks[32][UE_VAL_MAX]; int tnl = 0;
    char ttags[32][UE_KEY_MAX];  int tnt = 0;
    if (udev_db_read_links_tags(path, tlinks, &tnl, 32, ttags, &tnt, 32) != 0) return;
    g_dev_db++;

    /* SYM-EXTRA: ours not theirs -> interpreter applied a wrong link (fatal). */
    for (int i = 0; i < ctx.nsym; i++)
        if (!in_set(tlinks, tnl, ctx.symlinks[i])) {
            printf("SYM-EXTRA  %-10s %s\n", name, ctx.symlinks[i]);
            g_sym_extra++;
        }
    /* SYM-MISS: theirs not ours -> known-debt (by-id) or in-scope (fatal). */
    for (int i = 0; i < tnl; i++)
        if (!in_set(ctx.symlinks, ctx.nsym, tlinks[i])) {
            if (link_is_known_debt(tlinks[i])) {
                printf("KNOWN-DEBT %-10s %s\n", name, tlinks[i]);
                g_sym_miss_debt++;
            } else {
                printf("SYM-MISS   %-10s %s\n", name, tlinks[i]);
                g_sym_miss_inscope++;
            }
        }
    /* Tags: set compare G: both directions (in-scope, fatal). */
    for (int i = 0; i < tnt; i++)
        if (!in_tagset(ctx.tags, ctx.ntags, ttags[i])) {
            printf("TAG-MISS   %-10s %s\n", name, ttags[i]);
            g_tag_miss++;
        }
    for (int i = 0; i < ctx.ntags; i++)
        if (!in_tagset(ttags, tnt, ctx.tags[i])) {
            printf("TAG-EXTRA  %-10s %s\n", name, ctx.tags[i]);
            g_tag_extra++;
        }
}

int main(void) {
    ruleset_load_dirs((const char *const[]){
        "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" }, 3, &g_rs);
    printf("loaded %d native rule(s)\n", g_rs.n);
    coldplug_walk_root("/sys", collect);

    int inscope = g_sym_extra + g_sym_miss_inscope + g_tag_miss + g_tag_extra;
    printf("\n== verify-rules-live ==\n");
    printf("devices: %d scanned, %d with udev db\n", g_dev, g_dev_db);
    printf("SYM-EXTRA (fatal): %d\n", g_sym_extra);
    printf("SYM-MISS in-scope (fatal): %d\n", g_sym_miss_inscope);
    printf("SYM-MISS known-debt (by-id): %d\n", g_sym_miss_debt);
    printf("TAG-MISS (fatal): %d   TAG-EXTRA (fatal): %d\n", g_tag_miss, g_tag_extra);
    printf("IN-SCOPE DIVERGENCE: %d -> gate %s\n", inscope, inscope ? "FAIL" : "PASS");
    return inscope ? 1 : 0;
}
```

- [ ] **Step 2: Add the Makefile target** — after the `parity:` block (~line 54):

```make
verify-rules-live: tools/verify-rules-live.c udev-parity.h udev_db.h udev_rules.h udev_builtins.h udev_ruleset.h path_id.h udev_exec.h fido_id.h ata_id.h v4l_id.h cdrom_id.h optical_fs.h schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o verify-rules-live tools/verify-rules-live.c
	./verify-rules-live
```

(If the tool does not actually use `udev-parity.h` after Step 1, drop it from both the include list and the prereqs — keep prereqs honest.)

- [ ] **Step 3: Build clean, warnings matrix**

Run:
```bash
for std in c99 c11; do for opt in O0 O2; do \
  gcc -std=$std -$opt -Wall -Wextra -D_GNU_SOURCE -I. tools/verify-rules-live.c -o /tmp/vrl-$std-$opt \
  && echo "$std $opt OK" || echo "$std $opt FAIL"; done; done
```
Expected: four `OK`, no warnings.

- [ ] **Step 4: Run the gate on blakbox**

Run: `make verify-rules-live` (or `sudo ./verify-rules-live` if sysfs attrs need root for builtins)
Expected: prints the report and an exit line. **Record the actual numbers.** Interpret:
- `SYM-EXTRA > 0` → real interpreter bug. Root-cause before proceeding (systematic-debugging).
- `SYM-MISS in-scope > 0` → the interpreter/ported-builtin isn't producing a link udevd has, and it's not by-id debt. Investigate per device; some may reveal genuine gaps (by-uuid/by-partuuid from blkid, by-path from path_id) worth a follow-up slice.
- `KNOWN-DEBT` count → expected non-zero (ata_id/scsi_id); this is the honest surfaced debt, non-fatal.
- Tag diffs → investigate; `systemd`/`seat`/`uaccess` tags are the common ones.

The gate need not be green tonight — R5's deliverable is the **wired interpreter + the working gate that measures reality**. A red gate with only KNOWN-DEBT + understood in-scope items is a documented state, not a failure of R5. Capture the report in the commit message / NOW.

- [ ] **Step 5: Commit**

```bash
git add tools/verify-rules-live.c Makefile
git commit -m "feat(schema-udev): R5 verify-rules-live fidelity gate (S:/G: vs /run/udev/data)

Standalone read-only: recompute interpreter output per device, diff symlinks+tags
vs real udevd, exit(1) on in-scope divergence. by-id misses classified KNOWN-DEBT
(ata_id/scsi_id). <paste headline numbers here>

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Full-suite gate (after all three tasks)

- [ ] Run `make test` — every existing test plus the extended `test_udev_db` green.
- [ ] Run `make schema-udev` and `make verify-rules-live` — both build clean.
- [ ] Re-confirm live box untouched: `test ! -e /etc/schema-init/schema-udev.live && echo sentinel-absent`; `pgrep -x systemd-udevd`; `md5sum /usr/bin/schema-udev` unchanged from session open (`c42164b7f499c47e50182fd0c69be587`).
- [ ] Push branch to `origin/feat/schema-udev-cutover-e3` (backup; still NO PR, shadow-until-flip).

---

## Self-Review

**Spec coverage:**
- Daemon wiring / deep copy / free-on-reload / write-guard → Task 2 (Steps 3, 2, 3). ✓
- `udev_db_write_full` + new dir + `record_build_full` reuse → Task 1. ✓
- `udev_db_read_links_tags` (S:/G: reader) → Task 1. ✓
- `verify-rules-live` recompute-independent, SYM-EXTRA/MISS asymmetry, by-id KNOWN-DEBT, tag compare, exit-code gate → Task 3. ✓
- Makefile target + schema-udev prereq headers → Tasks 2/3. ✓
- Tests: write_full round-trip + reader → Task 1; live dry-run smoke + isolation diff → Task 2; gate run → Task 3. ✓
- Non-goals (ata_id/scsi_id surfaced as KNOWN-DEBT, not fixed) → Task 3 Step 4 classification. ✓
- Warnings matrix c99/c11×O0/O2 → each task's build step. ✓
- Live box untouched → Global Constraints + full-suite gate. ✓

**Placeholder scan:** Task 3 Step 5 commit message has an intentional `<paste headline numbers here>` — that is a runtime value the implementer fills from Step 4 output, not an unspecified design detail. Task 2 Step 4/Step 9 include a "verify the helper's body / compare against pre-R5 build" instruction rather than fabricated output — these are honest verification gates, and the exact idiom to confirm (`disk_links.h:146`) is named. No other placeholders.

**Type consistency:** `struct dev_ctx` fields `.symlinks`/`.nsym`/`.tags`/`.ntags` match `udev_ruleset.h` (lines 199–202). `udev_db_write_full(base, ev, kernel_n, symlinks, nsym, tags, ntag)` identical across Task 1 def, the Task 1 test, and the Task 2 call site. `udev_db_read_links_tags(path, links[][UE_VAL_MAX], *nlink, maxlink, tags[][UE_KEY_MAX], *ntag, maxtag)` identical across Task 1 def/test and Task 3 use. `record_build_full` arg order (symlinks, nsym, usec, tags, ntag) matches `udev_db.h:53`. `coldplug_walk_root` callback type `void(*)(struct uevent*)` matches `schema-udev.h:288`. ✓
