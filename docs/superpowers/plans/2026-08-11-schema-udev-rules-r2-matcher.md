# schema-udev Rule Interpreter — R2 Matcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evaluate udev match clauses against a device with faithful udev semantics — including the grouped parent-walk — as the R2 slice of the rule interpreter (spec `2026-08-11-schema-udev-rules-r2-matcher-design.md`).

**Architecture:** Extend the header-only `udev_ruleset.h` (built in R1) with four units: a udev glob matcher (`|` alternation over `fnmatch`), a match-time substitution expander, a `struct dev_ctx` device context, and `rule_match` with device-level keys plus the grouped parent-walk. A small `pi_driver` helper is added to `path_id.h`. R2 only reads device state; it executes nothing. Pure, shadow-only, no live writes.

**Tech Stack:** C11 / C99-compatible (`make` uses `-std=c99 -Wall -Wextra -D_GNU_SOURCE`), header-only `static inline`, assert-based tests wired into `make test`, `fnmatch` from libc. No external libraries.

## Global Constraints

- Header-only `static inline`; the only new `.c` is the test file. Matches `udev_ruleset.h`, `udev_db.h`, `schema-udev.h`.
- Compile clean (zero warnings) under both `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE` (manual task compiles) and the Makefile's `-std=c99 -Wall -Wextra -D_GNU_SOURCE`.
- No heap except R1's ruleset array; fixed buffers everywhere in R2.
- Use `safe_copy` (from `schema-udev.h`) for all bounded string copies.
- No live `/dev` or `/run/udev/data` writes introduced by this slice.
- Reuse R1 types (`struct rule`, `struct rule_clause`, `enum rule_op`, `ruleset_parse_line`) and existing sysfs primitives (`pi_parent`, `pi_sysattr`, `pi_subsystem`, `pi_base` in `path_id.h`; `uevent_get` in `schema-udev.h`).
- Constants: `DEVCTX_TAGS_MAX 32`.

---

## File Structure

- Modify: `path_id.h` — add `pi_driver` (readlink `<dir>/driver` → basename).
- Modify: `udev_ruleset.h` — add `udev_glob`, `ruleset_subst`, `struct dev_ctx`, `dev_ctx_init`, `rule_match` + helpers. Add `#include "path_id.h"`.
- Create: `tests/test_udev_matcher.c` — R2 TDD test binary (all four units + live smoke).
- Modify: `Makefile` — add the matcher test to the `test:` target.

R2 test file is separate from R1's `tests/test_udev_ruleset.c`: the parser and the matcher are independently reviewable and the split keeps each `main()` focused.

---

### Task 1: `udev_glob` — glob with `|` alternation

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_matcher.c` (create)

**Interfaces:**
- Produces: `int udev_glob(const char *pat, const char *str)` — returns 1 if `str` matches udev glob `pat`. Splits `pat` on top-level `|` (alternation), respecting `[...]` bracket classes (a `|` inside a class is literal), and returns 1 if any alternative satisfies `fnmatch(alt, str, 0)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_udev_matcher.c`:

```c
#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

int main(void) {
    /* alternation */
    assert(udev_glob("sd*|vd*", "sda") == 1);
    assert(udev_glob("sd*|vd*", "vdb") == 1);
    assert(udev_glob("sd*|vd*", "hda") == 0);
    /* globs delegated to fnmatch */
    assert(udev_glob("sd[a-c]", "sdb") == 1);
    assert(udev_glob("sd[a-c]", "sdd") == 0);
    assert(udev_glob("tty?", "ttyS") == 1);
    assert(udev_glob("event[0-9]", "event3") == 1);
    /* a '|' inside a bracket class is NOT an alternation split */
    assert(udev_glob("a[b|c]d", "abd") == 1);
    assert(udev_glob("a[b|c]d", "a|d") == 1);
    /* exact */
    assert(udev_glob("exact", "exact") == 1);
    assert(udev_glob("exact", "other") == 0);

    printf("test_udev_matcher: glob OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher`
Expected: FAIL — `udev_glob` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` before `#endif` (fnmatch comes from `schema-udev.h`):

```c
/* udev glob: split PAT on top-level '|' (respecting [..] classes), fnmatch each. */
static inline int udev_glob(const char *pat, const char *str) {
    const char *seg = pat, *p = pat;
    int inbr = 0;
    for (;; p++) {
        if (*p == '[') inbr = 1;
        else if (*p == ']') inbr = 0;
        if (*p == '\0' || (*p == '|' && !inbr)) {
            size_t len = (size_t)(p - seg);
            char buf[RK_VAL_MAX];
            if (len < sizeof buf) {
                memcpy(buf, seg, len); buf[len] = '\0';
                if (fnmatch(buf, str, 0) == 0) return 1;
            }
            if (*p == '\0') break;
            seg = p + 1;
        }
    }
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: `test_udev_matcher: glob OK`

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_matcher.c
git commit -m "feat(schema-udev): R2 udev_glob (| alternation over fnmatch)"
```

---

### Task 2: `pi_driver` + `struct dev_ctx` + `dev_ctx_init`

**Files:**
- Modify: `path_id.h` (add `pi_driver`)
- Modify: `udev_ruleset.h` (add `#include "path_id.h"`, `struct dev_ctx`, `dev_ctx_init`)
- Test: `tests/test_udev_matcher.c`

**Interfaces:**
- Produces:
  - `int pi_driver(const char *devdir, char *out, size_t outsz)` — `readlink` `<devdir>/driver`, write basename to `out`. Returns 0 / -1. Mirrors `pi_subsystem`.
  - `struct dev_ctx { struct uevent *ev; const char *sysroot; char sysdir[PATH_MAX]; char tags[DEVCTX_TAGS_MAX][UE_KEY_MAX]; int ntags; char matched_parent[UE_KEY_MAX]; }`.
  - `int dev_ctx_init(struct dev_ctx *ctx, struct uevent *ev, const char *sysroot)` — zero `ctx`, set `ev`/`sysroot`, compute `sysdir = sysroot + DEVPATH`. Returns 0 / -1 (no DEVPATH or overflow).

- [ ] **Step 1: Write the failing test**

Add a `ue_set` helper above `main` (used by later tasks too) and a Task-2 block. Put the helper right after the includes:

```c
static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}
```

Add before the final `printf`/`return` in `main`:

```c
    /* pi_driver: create <dir>/driver symlink, expect basename */
    char t2[] = "/tmp/schema-m2-XXXXXX"; assert(mkdtemp(t2));
    char dvd[PATH_MAX]; snprintf(dvd, sizeof dvd, "%s/dev", t2); assert(mkdir(dvd, 0755) == 0);
    char dl[PATH_MAX]; snprintf(dl, sizeof dl, "%s/driver", dvd);
    assert(symlink("../../bus/pci/drivers/ahci", dl) == 0);
    char drv[64]; assert(pi_driver(dvd, drv, sizeof drv) == 0 && strcmp(drv, "ahci") == 0);
    char dvd2[PATH_MAX]; snprintf(dvd2, sizeof dvd2, "%s/nodrv", t2); assert(mkdir(dvd2, 0755) == 0);
    assert(pi_driver(dvd2, drv, sizeof drv) == -1);

    /* dev_ctx_init: sysdir = sysroot + DEVPATH */
    struct uevent ev2; memset(&ev2, 0, sizeof ev2);
    ue_set(&ev2, "ACTION", "add");
    ue_set(&ev2, "DEVPATH", "/devices/pci/block/sda");
    struct dev_ctx ctx2;
    assert(dev_ctx_init(&ctx2, &ev2, "/sys") == 0);
    assert(strcmp(ctx2.sysdir, "/sys/devices/pci/block/sda") == 0);
    assert(ctx2.ntags == 0 && ctx2.matched_parent[0] == '\0' && ctx2.ev == &ev2);
    struct uevent ev3; memset(&ev3, 0, sizeof ev3); ue_set(&ev3, "ACTION", "add");
    assert(dev_ctx_init(&ctx2, &ev3, "/sys") == -1);   /* no DEVPATH */
    unlink(dl); rmdir(dvd); rmdir(dvd2); rmdir(t2);

    printf("test_udev_matcher: ctx OK\n");
```

(Move the existing `printf("test_udev_matcher: glob OK\n"); return 0;` so `glob OK` prints first and `return 0;` stays last; keep all prints.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher`
Expected: FAIL — `pi_driver` / `dev_ctx` / `dev_ctx_init` undefined.

- [ ] **Step 3: Write minimal implementation**

In `path_id.h`, after `pi_subsystem` (before `pi_sysattr` is fine), add:

```c
static inline int pi_driver(const char *devdir, char *out, size_t outsz) {
    char link[PATH_MAX], target[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "%s/driver", devdir) >= sizeof link) return -1;
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0) return -1;
    target[n] = '\0';
    char *b = strrchr(target, '/');
    safe_copy(out, b ? b + 1 : target, outsz);
    return 0;
}
```

In `udev_ruleset.h`, add the include near the top (after `#include "schema-udev.h"`):

```c
#include "path_id.h"   /* pi_parent, pi_sysattr, pi_subsystem, pi_base, pi_driver */
```

Then add before `#endif`:

```c
#define DEVCTX_TAGS_MAX 32

struct dev_ctx {
    struct uevent *ev;                          /* properties; mutable (R3 grows) */
    const char    *sysroot;                     /* e.g. "/sys" */
    char           sysdir[PATH_MAX];            /* absolute sysfs dir of device */
    char           tags[DEVCTX_TAGS_MAX][UE_KEY_MAX];
    int            ntags;
    char           matched_parent[UE_KEY_MAX];  /* last parent-group match kname */
};

static inline int dev_ctx_init(struct dev_ctx *ctx, struct uevent *ev, const char *sysroot) {
    memset(ctx, 0, sizeof *ctx);
    ctx->ev = ev;
    ctx->sysroot = sysroot;
    const char *dp = uevent_get(ev, "DEVPATH");
    if (!dp) return -1;
    if ((size_t)snprintf(ctx->sysdir, sizeof ctx->sysdir, "%s%s", sysroot, dp) >= sizeof ctx->sysdir)
        return -1;
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: `glob OK` then `ctx OK`.

- [ ] **Step 5: Commit**

```bash
git add path_id.h udev_ruleset.h tests/test_udev_matcher.c
git commit -m "feat(schema-udev): R2 pi_driver + struct dev_ctx + dev_ctx_init"
```

---

### Task 3: `ruleset_subst` — match-resolvable substitutions

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_matcher.c`

**Interfaces:**
- Consumes: `struct dev_ctx`, `uevent_get`, `pi_base`, `pi_sysattr`.
- Produces: `int ruleset_subst(const char *in, const struct dev_ctx *ctx, char *out, size_t sz)` — expand match-resolvable tokens into `out` (bounded by `sz`). Resolvable: `%k/$kernel`, `%n/$number`, `%p/$devpath`, `%b/$id`, `%M/$major`, `%m/$minor`, `$driver`, `%S/$sys`, `%r/$root`, `%E{K}/$env{K}`, `%s{F}/$attr{F}`, `%%`, `$$`. Deferred tokens (`$result`/`%c`, `$links`, `$name`, `$parent`/`%P`, and any unknown) are copied verbatim. Returns 0.

- [ ] **Step 1: Write the failing test**

Add before the final print:

```c
    struct uevent evs; memset(&evs, 0, sizeof evs);
    ue_set(&evs, "ACTION", "add");
    ue_set(&evs, "DEVPATH", "/devices/pci/ata1/block/sda/sda3");
    ue_set(&evs, "MAJOR", "8");
    ue_set(&evs, "MINOR", "3");
    ue_set(&evs, "ID_BUS", "ata");
    struct dev_ctx cs; assert(dev_ctx_init(&cs, &evs, "/sys") == 0);
    char o[256];

    ruleset_subst("k=%k n=%n M=%M m=%m", &cs, o, sizeof o);
    assert(strcmp(o, "k=sda3 n=3 M=8 m=3") == 0);
    ruleset_subst("$env{ID_BUS}-$kernel", &cs, o, sizeof o);
    assert(strcmp(o, "ata-sda3") == 0);
    ruleset_subst("p=$devpath", &cs, o, sizeof o);
    assert(strcmp(o, "p=/devices/pci/ata1/block/sda/sda3") == 0);
    ruleset_subst("100%%$$done", &cs, o, sizeof o);
    assert(strcmp(o, "100%$done") == 0);
    /* deferred tokens copied verbatim */
    ruleset_subst("x$result-$links-%c-$name", &cs, o, sizeof o);
    assert(strcmp(o, "x$result-$links-%c-$name") == 0);
    /* $id / %b reads matched_parent */
    safe_copy(cs.matched_parent, "0000:00:1f.2", sizeof cs.matched_parent);
    ruleset_subst("$id|%b", &cs, o, sizeof o);
    assert(strcmp(o, "0000:00:1f.2|0000:00:1f.2") == 0);

    printf("test_udev_matcher: subst OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher`
Expected: FAIL — `ruleset_subst` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` before `#endif`:

```c
static inline void rs_app(char *out, size_t sz, size_t *o, const char *s) {
    if (!s) return;
    while (*s && *o + 1 < sz) out[(*o)++] = *s++;
    out[*o] = '\0';
}

/* Expand match-resolvable substitution tokens; deferred/unknown copied verbatim. */
static inline int ruleset_subst(const char *in, const struct dev_ctx *ctx, char *out, size_t sz) {
    size_t o = 0; if (sz) out[0] = '\0';
    const char *dp = uevent_get(ctx->ev, "DEVPATH");
    const char *kname = dp ? pi_base(dp) : "";
    for (const char *p = in; *p; ) {
        if (*p != '$' && *p != '%') { if (o + 1 < sz) { out[o++] = *p; out[o] = '\0'; } p++; continue; }
        char sig = *p;
        if (sig == '$' && p[1] == '$') { rs_app(out, sz, &o, "$"); p += 2; continue; }
        if (sig == '%' && p[1] == '%') { rs_app(out, sz, &o, "%"); p += 2; continue; }
        const char *q = p + 1;
        char name[32]; size_t nl = 0;
        if (sig == '$') { while (((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) && nl < sizeof name - 1) name[nl++] = *q++; }
        else if (*q) { name[nl++] = *q++; }   /* % form: single letter */
        name[nl] = '\0';
        char arg[128]; arg[0] = '\0';
        if (*q == '{') { const char *e = strchr(q, '}');
            if (e) { size_t al = (size_t)(e - (q + 1)); if (al < sizeof arg) { memcpy(arg, q + 1, al); arg[al] = '\0'; } q = e + 1; } }
        char tmp[UE_VAL_MAX];
        const char *rep = NULL; int known = 1;
        if      ((sig == '$' && !strcmp(name, "kernel"))  || (sig == '%' && !strcmp(name, "k"))) rep = kname;
        else if ((sig == '$' && !strcmp(name, "number"))  || (sig == '%' && !strcmp(name, "n"))) {
            const char *d = kname + strlen(kname); while (d > kname && d[-1] >= '0' && d[-1] <= '9') d--; rep = d; }
        else if ((sig == '$' && !strcmp(name, "devpath")) || (sig == '%' && !strcmp(name, "p"))) rep = dp ? dp : "";
        else if ((sig == '$' && !strcmp(name, "id"))      || (sig == '%' && !strcmp(name, "b"))) rep = ctx->matched_parent;
        else if ((sig == '$' && !strcmp(name, "major"))   || (sig == '%' && !strcmp(name, "M"))) rep = uevent_get(ctx->ev, "MAJOR");
        else if ((sig == '$' && !strcmp(name, "minor"))   || (sig == '%' && !strcmp(name, "m"))) rep = uevent_get(ctx->ev, "MINOR");
        else if  (sig == '$' && !strcmp(name, "driver"))  rep = uevent_get(ctx->ev, "DRIVER");
        else if ((sig == '$' && !strcmp(name, "sys"))     || (sig == '%' && !strcmp(name, "S"))) rep = ctx->sysroot;
        else if ((sig == '$' && !strcmp(name, "root"))    || (sig == '%' && !strcmp(name, "r"))) rep = "/dev";
        else if ((sig == '$' && !strcmp(name, "env"))     || (sig == '%' && !strcmp(name, "E"))) rep = uevent_get(ctx->ev, arg);
        else if ((sig == '$' && !strcmp(name, "attr"))    || (sig == '%' && !strcmp(name, "s"))) {
            rep = (pi_sysattr(ctx->sysdir, arg, tmp, sizeof tmp) == 0) ? tmp : ""; }
        else known = 0;
        if (known) { rs_app(out, sz, &o, rep ? rep : ""); p = q; }
        else {
            /* deferred/unknown token: copy [p, q) verbatim */
            size_t tl = (size_t)(q - p); char tk[160];
            if (tl < sizeof tk) { memcpy(tk, p, tl); tk[tl] = '\0'; rs_app(out, sz, &o, tk); }
            p = q;
        }
    }
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: `subst OK` printed, no assert failures.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_matcher.c
git commit -m "feat(schema-udev): R2 ruleset_subst (match-resolvable subset)"
```

---

### Task 4: `rule_match` — device-level match keys

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_matcher.c`

**Interfaces:**
- Consumes: `udev_glob`, `struct dev_ctx`, `uevent_get`, `pi_base`, `pi_sysattr`.
- Produces:
  - `int rk_cmp(enum rule_op op, const char *pat, const char *actual)` — glob compare with `!=` negation; `actual == NULL` fails `==`, satisfies `!=`.
  - `int match_dev_clause(const struct rule_clause *c, const struct dev_ctx *ctx)` — 1 match / 0 nomatch / **-1 not a device-level key** (parent key, or R4 conditional). Handles `ACTION`, `DEVPATH`, `SUBSYSTEM`, `KERNEL`, `DRIVER`, `ENV`, `ATTR`, `TAG`.
  - `int rule_match(const struct rule *r, struct dev_ctx *ctx)` — 1 if all match-op clauses pass, else 0. Assignment ops skipped. In this task, non-device match keys (the `-1` case) are skipped (parent-walk added in Task 5).

- [ ] **Step 1: Write the failing test**

Add before the final print:

```c
    /* device-level matching, incl. an ATTR read from a synthetic sysdir */
    char t4[] = "/tmp/schema-m4-XXXXXX"; assert(mkdtemp(t4));
    char xdir[PATH_MAX]; snprintf(xdir, sizeof xdir, "%s/devices", t4); assert(mkdir(xdir, 0755) == 0);
    snprintf(xdir, sizeof xdir, "%s/devices/sda", t4); assert(mkdir(xdir, 0755) == 0);
    char af[PATH_MAX]; snprintf(af, sizeof af, "%s/devices/sda/serial", t4);
    FILE *sf = fopen(af, "w"); fputs("ABC123\n", sf); fclose(sf);

    struct uevent evm; memset(&evm, 0, sizeof evm);
    ue_set(&evm, "ACTION", "add");
    ue_set(&evm, "DEVPATH", "/devices/sda");
    ue_set(&evm, "SUBSYSTEM", "block");
    ue_set(&evm, "DRIVER", "sd");
    ue_set(&evm, "ID_FS_TYPE", "ext4");
    struct dev_ctx cm; assert(dev_ctx_init(&cm, &evm, t4) == 0);
    safe_copy(cm.tags[cm.ntags++], "systemd", UE_KEY_MAX);

    struct rule r;
    ruleset_parse_line("ACTION==\"add\", SUBSYSTEM==\"block\", KERNEL==\"sd*\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("SUBSYSTEM==\"net\"", &r);
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("ACTION!=\"remove\", ENV{ID_FS_TYPE}==\"ext4\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ENV{NOPE}==\"x\"", &r);          /* missing => == fails */
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("ENV{NOPE}!=\"x\"", &r);          /* missing => != passes */
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ATTR{serial}==\"ABC123\"", &r);  /* sysfs attr read */
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("ATTR{serial}==\"WRONG\"", &r);
    assert(rule_match(&r, &cm) == 0);
    ruleset_parse_line("TAG==\"systemd\"", &r);
    assert(rule_match(&r, &cm) == 1);
    ruleset_parse_line("TAG==\"seat\"", &r);
    assert(rule_match(&r, &cm) == 0);
    /* assignment clauses are ignored by the matcher */
    ruleset_parse_line("SUBSYSTEM==\"block\", SYMLINK+=\"disk/by-x\"", &r);
    assert(rule_match(&r, &cm) == 1);

    unlink(af); rmdir(xdir);
    snprintf(xdir, sizeof xdir, "%s/devices", t4); rmdir(xdir); rmdir(t4);

    printf("test_udev_matcher: dev-match OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher`
Expected: FAIL — `rule_match` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` before `#endif`:

```c
static inline int rk_is_match_op(enum rule_op op) { return op == OP_MATCH_EQ || op == OP_MATCH_NE; }

static inline int rk_cmp(enum rule_op op, const char *pat, const char *actual) {
    int m = (actual != NULL) && udev_glob(pat, actual);
    return (op == OP_MATCH_NE) ? !m : m;
}

/* 1 match / 0 nomatch / -1 not a device-level key */
static inline int match_dev_clause(const struct rule_clause *c, const struct dev_ctx *ctx) {
    const struct uevent *ev = ctx->ev;
    if (!strcmp(c->key, "ACTION"))    return rk_cmp(c->op, c->val, uevent_get(ev, "ACTION"));
    if (!strcmp(c->key, "DEVPATH"))   return rk_cmp(c->op, c->val, uevent_get(ev, "DEVPATH"));
    if (!strcmp(c->key, "SUBSYSTEM")) return rk_cmp(c->op, c->val, uevent_get(ev, "SUBSYSTEM"));
    if (!strcmp(c->key, "DRIVER"))    return rk_cmp(c->op, c->val, uevent_get(ev, "DRIVER"));
    if (!strcmp(c->key, "KERNEL"))    { const char *dp = uevent_get(ev, "DEVPATH");
                                        return rk_cmp(c->op, c->val, dp ? pi_base(dp) : NULL); }
    if (!strcmp(c->key, "ENV"))       return rk_cmp(c->op, c->val, uevent_get(ev, c->subkey));
    if (!strcmp(c->key, "ATTR"))      { char b[UE_VAL_MAX];
                                        int ok = pi_sysattr(ctx->sysdir, c->subkey, b, sizeof b) == 0;
                                        return rk_cmp(c->op, c->val, ok ? b : NULL); }
    if (!strcmp(c->key, "TAG")) {
        int has = 0;
        for (int i = 0; i < ctx->ntags; i++) if (udev_glob(c->val, ctx->tags[i])) { has = 1; break; }
        return c->op == OP_MATCH_NE ? !has : has;
    }
    return -1;   /* parent key (SUBSYSTEMS/…) or R4 conditional (TEST/PROGRAM) */
}

static inline int rule_match(const struct rule *r, struct dev_ctx *ctx) {
    for (int i = 0; i < r->nclause; i++) {
        const struct rule_clause *c = &r->clause[i];
        if (!rk_is_match_op(c->op)) continue;    /* assignments: R3 */
        int d = match_dev_clause(c, ctx);
        if (d == 0) return 0;
        if (d == 1) continue;
        /* d == -1: parent-match group handled in Task 5; other conditionals (R4) skipped */
        continue;
    }
    return 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: `dev-match OK`, no assert failures.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_matcher.c
git commit -m "feat(schema-udev): R2 rule_match device-level keys"
```

---

### Task 5: grouped parent-walk (`SUBSYSTEMS`/`KERNELS`/`DRIVERS`/`ATTRS`/`TAGS`)

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_matcher.c`

**Interfaces:**
- Consumes: `rk_cmp`, `udev_glob`, `pi_parent`, `pi_subsystem`, `pi_driver`, `pi_sysattr`, `pi_base`.
- Produces:
  - `int rk_is_parent_key(const char *k)` — true for `SUBSYSTEMS`/`KERNELS`/`DRIVERS`/`ATTRS`/`TAGS`.
  - `int parent_clause_on(const struct rule_clause *c, const char *anc, const struct dev_ctx *ctx)` — does ancestor dir `anc` satisfy this single parent clause?
  - `int parent_group_match(const struct rule_clause *cl, int nc, struct dev_ctx *ctx)` — climb from `ctx->sysdir` (self included) via `pi_parent`; 1 iff some single ancestor satisfies **all** `nc` clauses, storing that ancestor's basename in `ctx->matched_parent`. 0 otherwise.
  - `rule_match` updated: collect consecutive parent-match clauses into a group and require `parent_group_match`.

- [ ] **Step 1: Write the failing test**

Add before the final print. This builds a two-level synthetic tree (parent `A` = pci/ahci/vendor, device `B` = block) and proves the grouped semantics — the crux is that clauses satisfiable only across *different* ancestors must NOT match.

```c
    /* synthetic tree: <root>/devices/A/B ; A=pci+ahci+vendor, B=block */
    char t5[] = "/tmp/schema-m5-XXXXXX"; assert(mkdtemp(t5));
    char pp[PATH_MAX];
    snprintf(pp, sizeof pp, "%s/devices", t5);       assert(mkdir(pp, 0755) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A", t5);     assert(mkdir(pp, 0755) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A/B", t5);   assert(mkdir(pp, 0755) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A/subsystem", t5);  assert(symlink("../../class/pci", pp) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A/driver", t5);     assert(symlink("../../bus/ahci", pp) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A/vendor", t5);
    { FILE *f = fopen(pp, "w"); fputs("0x8086\n", f); fclose(f); }
    snprintf(pp, sizeof pp, "%s/devices/A/B/subsystem", t5); assert(symlink("../../../class/block", pp) == 0);
    snprintf(pp, sizeof pp, "%s/devices/A/B/onlyB", t5);
    { FILE *f = fopen(pp, "w"); fputs("x\n", f); fclose(f); }

    struct uevent evp; memset(&evp, 0, sizeof evp);
    ue_set(&evp, "ACTION", "add");
    ue_set(&evp, "DEVPATH", "/devices/A/B");
    ue_set(&evp, "SUBSYSTEM", "block");
    struct dev_ctx cp; assert(dev_ctx_init(&cp, &evp, t5) == 0);
    struct rule pr;

    /* all three satisfied by ancestor A -> match, matched_parent == "A" */
    ruleset_parse_line("SUBSYSTEMS==\"pci\", DRIVERS==\"ahci\", ATTRS{vendor}==\"0x8086\"", &pr);
    assert(rule_match(&pr, &cp) == 1);
    assert(strcmp(cp.matched_parent, "A") == 0);

    /* value mismatch on the same ancestor -> no match */
    ruleset_parse_line("SUBSYSTEMS==\"pci\", ATTRS{vendor}==\"0xbeef\"", &pr);
    assert(rule_match(&pr, &cp) == 0);

    /* THE CRUX: clauses satisfiable only across DIFFERENT ancestors must NOT match
       (A has pci, B has onlyB; no single ancestor has both) */
    ruleset_parse_line("SUBSYSTEMS==\"pci\", ATTRS{onlyB}==\"x\"", &pr);
    assert(rule_match(&pr, &cp) == 0);

    /* device self is included in the walk: SUBSYSTEMS matches B's own subsystem */
    ruleset_parse_line("SUBSYSTEMS==\"block\"", &pr);
    assert(rule_match(&pr, &cp) == 1);

    /* device-level and parent-group clauses combine correctly */
    ruleset_parse_line("KERNEL==\"B\", SUBSYSTEMS==\"pci\", DRIVERS==\"ahci\"", &pr);
    assert(rule_match(&pr, &cp) == 1);

    /* cleanup */
    snprintf(pp, sizeof pp, "%s/devices/A/B/subsystem", t5); unlink(pp);
    snprintf(pp, sizeof pp, "%s/devices/A/B/onlyB", t5);     unlink(pp);
    snprintf(pp, sizeof pp, "%s/devices/A/B", t5);           rmdir(pp);
    snprintf(pp, sizeof pp, "%s/devices/A/subsystem", t5);   unlink(pp);
    snprintf(pp, sizeof pp, "%s/devices/A/driver", t5);      unlink(pp);
    snprintf(pp, sizeof pp, "%s/devices/A/vendor", t5);      unlink(pp);
    snprintf(pp, sizeof pp, "%s/devices/A", t5);             rmdir(pp);
    snprintf(pp, sizeof pp, "%s/devices", t5);               rmdir(pp);
    rmdir(t5);

    printf("test_udev_matcher: parent-walk OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: FAIL — the crux assertion (`SUBSYSTEMS==pci, ATTRS{onlyB}==x` → 0) fails, because Task 4's `rule_match` skips parent keys and returns 1.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` before `#endif`:

```c
static inline int rk_is_parent_key(const char *k) {
    return !strcmp(k, "SUBSYSTEMS") || !strcmp(k, "KERNELS") ||
           !strcmp(k, "DRIVERS")    || !strcmp(k, "ATTRS")   || !strcmp(k, "TAGS");
}

static inline int parent_clause_on(const struct rule_clause *c, const char *anc,
                                   const struct dev_ctx *ctx) {
    char buf[UE_VAL_MAX];
    if (!strcmp(c->key, "SUBSYSTEMS"))
        return rk_cmp(c->op, c->val, pi_subsystem(anc, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "KERNELS"))
        return rk_cmp(c->op, c->val, pi_base(anc));
    if (!strcmp(c->key, "DRIVERS"))
        return rk_cmp(c->op, c->val, pi_driver(anc, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "ATTRS"))
        return rk_cmp(c->op, c->val, pi_sysattr(anc, c->subkey, buf, sizeof buf) == 0 ? buf : NULL);
    if (!strcmp(c->key, "TAGS")) {
        int has = 0;
        for (int i = 0; i < ctx->ntags; i++) if (udev_glob(c->val, ctx->tags[i])) { has = 1; break; }
        return c->op == OP_MATCH_NE ? !has : has;
    }
    return 0;
}

/* climb from ctx->sysdir (self first) up to sysroot; match iff one ancestor
 * satisfies ALL nc clauses. Records that ancestor's basename in matched_parent. */
static inline int parent_group_match(const struct rule_clause *cl, int nc, struct dev_ctx *ctx) {
    char anc[PATH_MAX];
    safe_copy(anc, ctx->sysdir, sizeof anc);
    for (;;) {
        int all = 1;
        for (int k = 0; k < nc; k++)
            if (!parent_clause_on(&cl[k], anc, ctx)) { all = 0; break; }
        if (all) { safe_copy(ctx->matched_parent, pi_base(anc), UE_KEY_MAX); return 1; }
        if (strlen(anc) <= strlen(ctx->sysroot)) return 0;   /* don't climb above sysroot */
        if (pi_parent(anc) != 0) return 0;
    }
}
```

Then replace the `d == -1` tail of `rule_match` (the `/* d == -1: … */` comment and its `continue;`) with:

```c
        /* d == -1: a parent-match group, or an R4 conditional (TEST/PROGRAM) */
        if (rk_is_parent_key(c->key)) {
            int j = i;
            while (j < r->nclause && rk_is_match_op(r->clause[j].op) &&
                   rk_is_parent_key(r->clause[j].key)) j++;
            if (!parent_group_match(&r->clause[i], j - i, ctx)) return 0;
            i = j - 1;   /* for-loop ++ advances past the group */
            continue;
        }
        continue;   /* unknown match key (TEST/PROGRAM): deferred to R4 */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_matcher.c -o /tmp/t-matcher && /tmp/t-matcher`
Expected: `parent-walk OK`, all asserts pass (crux returns 0).

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_matcher.c
git commit -m "feat(schema-udev): R2 grouped parent-walk matcher"
```

---

### Task 6: Wire into `make test` + live smoke

**Files:**
- Modify: `Makefile` (`test:` target)
- Test: `tests/test_udev_matcher.c`

**Interfaces:**
- Consumes: everything above + R1 `ruleset_load_dirs`.
- Produces: no new function — a live smoke that `rule_match` gives known-true/known-false answers on a real device, and that running the full installed ruleset against a real device does not crash.

- [ ] **Step 1: Write the failing test**

Add before the final `printf("...ALL OK...")`/`return`:

```c
    /* live smoke: rule_match on a real block device if present */
    if (access("/sys/block/sda", F_OK) == 0) {
        char lnk[PATH_MAX]; ssize_t ln = readlink("/sys/block/sda", lnk, sizeof lnk - 1);
        assert(ln > 0); lnk[ln] = '\0';
        const char *dp = strstr(lnk, "/devices/");
        assert(dp != NULL);
        struct uevent el; memset(&el, 0, sizeof el);
        ue_set(&el, "ACTION", "add");
        ue_set(&el, "DEVPATH", dp);
        ue_set(&el, "SUBSYSTEM", "block");
        struct dev_ctx cl; assert(dev_ctx_init(&cl, &el, "/sys") == 0);

        struct rule r;
        ruleset_parse_line("SUBSYSTEM==\"block\", KERNEL==\"sda\"", &r);
        assert(rule_match(&r, &cl) == 1);          /* known-true */
        ruleset_parse_line("SUBSYSTEM==\"net\"", &r);
        assert(rule_match(&r, &cl) == 0);          /* known-false */

        /* run the whole installed ruleset against the real device: must not crash */
        const char *real[] = { "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };
        struct ruleset live = {0};
        assert(ruleset_load_dirs(real, 3, &live) == 0);
        int matched = 0;
        for (int i = 0; i < live.n; i++) if (rule_match(&live.rules[i], &cl)) matched++;
        assert(matched >= 0);
        free(live.rules);
    }

    printf("test_udev_matcher: ALL OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | grep -i matcher`
Expected: FAIL — no `matcher` line runs (test not in `test:` target yet).

- [ ] **Step 3: Add the test to the Makefile**

In the `test:` target, after the `test_udev_ruleset` line, add:

```make
	$(CC) $(CFLAGS) tests/test_udev_matcher.c -o /tmp/schema-test-matcher && /tmp/schema-test-matcher
```

- [ ] **Step 4: Run the full suite to verify it passes**

Run: `make test 2>&1 | tail -6`
Expected: `test_udev_matcher: ALL OK` and no failures across the suite.

- [ ] **Step 5: Commit**

```bash
git add Makefile tests/test_udev_matcher.c
git commit -m "test(schema-udev): wire R2 matcher into make test + live smoke"
```

---

## Self-Review

**Spec coverage:**
- Match `ACTION/SUBSYSTEM/KERNEL/ENV/ATTR/DRIVER/TAG` on the device → Task 4. ✓
- Parent-walking `SUBSYSTEMS/KERNELS/ATTRS/DRIVERS/TAGS` with grouped semantics → Task 5. ✓
- Glob `* ? [...]` + `|` alternation → Task 1. ✓
- Substitutions (match-resolvable subset; deferred verbatim) → Task 3. ✓
- `$id`/`%b` via matched-parent; `pi_driver` symlink read → Tasks 2/3/5. ✓
- Shadow-only, no live writes → all tasks (only reads + tmp fixtures). ✓
- Live smoke, anti-hollow (≥1 true, ≥1 false); full-record fidelity deferred to R5 → Task 6. ✓

**Placeholder scan:** No TBD/TODO; every code step is real code. The Task-4 `rule_match` `d == -1` skip is explicitly completed in Task 5 (its code shown in full there), not a placeholder. ✓

**Type consistency:** `struct dev_ctx`, `struct rule`/`struct rule_clause`/`enum rule_op` (R1), and signatures `udev_glob`, `ruleset_subst`, `dev_ctx_init`, `pi_driver`, `rk_cmp`, `rk_is_match_op`, `match_dev_clause`, `rule_match`, `rk_is_parent_key`, `parent_clause_on`, `parent_group_match` are defined once and used consistently. `ue_set` is a test-only helper defined at the top of `tests/test_udev_matcher.c`. ✓

**Known follow-ups (not R2):** executor + `GOTO`/`LABEL` + `TAG+=`/`SYMLINK+=` accumulation (R3); `IMPORT`/`RUN`/`TEST`/`PROGRAM` and the deferred substitutions `$result`/`$links`/`$name`/`$parent` (R4); full-record fidelity gate across all 471 devices (R5).
