# schema-udev R3 Executor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the udev rule-interpreter *executor* — apply matched rules' assignment clauses (ENV/TAG/SYMLINK/OPTIONS/MODE/GROUP/OWNER/NAME) with GOTO/LABEL flow, accumulating the device's tag set, symlink set, and node perms into `struct dev_ctx`.

**Architecture:** All logic lives in the existing header `udev_ruleset.h`, extending R1/R2 structures. Small pure helpers (`uevent_set`, `ctx_*` tag/symlink ops, `udev_replace_chars`, `apply_options`, final-lock) are composed by `apply_rule` (one rule's assignments) and driven by `ruleset_apply` (walks the ruleset in order with GOTO/LABEL). Shadow-only: udevd stays authoritative, nothing writes `/dev` or `/run/udev/data`.

**Tech Stack:** C (C11 and C99), header-only static-inline functions, the existing `make test` assert-driven harness (`tests/test_udev_executor.c`).

## Global Constraints

- **Compiles clean under BOTH `-std=c11` and `-std=c99`, `-Wall -Wextra`, ZERO warnings.** (The `make test` target runs both; a warning is a failure.)
- **Header-only:** all new code is `static inline` in `udev_ruleset.h`. No new `.c` translation unit.
- **Shadow-only:** no live `/dev` node, no `/run/udev/data` write, no process started from a working tree. The built binary is gitignored and not installed.
- **The `schema-udev` daemon must still build** after every task (`make schema-udev`) — it does not yet call `ruleset_apply`; the header addition must not break it.
- **Reuse R1/R2 primitives:** `ruleset_parse_line`, `ruleset_append`, `rule_match`, `ruleset_subst`, `udev_glob`, `safe_copy`, `uevent_get`. Do not duplicate them.
- Bounds: `DEVCTX_TAGS_MAX` 32 (existing), `DEVCTX_SYMLINKS_MAX` 32, `DEVCTX_FINAL_MAX` 16. Over-bound adds are silently dropped (never overflow).

---

## File Structure

- **Modify** `udev_ruleset.h` — grow `struct dev_ctx`; add `uevent_set`, the `ctx_*` helpers, `udev_replace_chars`, `apply_options`, final-lock helpers, `apply_rule`, `ruleset_apply`; edit `rule_match` to maintain `last_rule_deferred`.
- **Create** `tests/test_udev_executor.c` — the R3 test main; grown task-by-task.
- **Modify** `Makefile` — add the executor test to the `test` target (next to the ruleset/matcher lines ~102-103).

---

### Task 1: dev_ctx growth + `uevent_set`

**Files:**
- Modify: `udev_ruleset.h` (`struct dev_ctx` ~line 188; new `uevent_set` after the `#include`s / near `dev_ctx_init`)
- Create: `tests/test_udev_executor.c`
- Modify: `Makefile` (add executor test line under the ruleset/matcher lines ~102)

**Interfaces:**
- Consumes: `struct uevent` (`schema-udev.h`: `key[UE_MAX_KEYS][UE_KEY_MAX]`, `val[UE_MAX_KEYS][UE_VAL_MAX]`, `int n`), `uevent_get`, `safe_copy`, `struct dev_ctx`, `dev_ctx_init`, `ruleset_parse_line`, `rule_match`.
- Produces:
  - `struct dev_ctx` gains: `char symlinks[DEVCTX_SYMLINKS_MAX][UE_VAL_MAX]; int nsym; char mode[8]; char group[UE_KEY_MAX]; char owner[UE_KEY_MAX]; char name[UE_VAL_MAX]; int link_priority; int escape; char final_keys[DEVCTX_FINAL_MAX][RK_KEY_MAX+RK_SUB_MAX+2]; int nfinal; int last_rule_deferred; int deferred_applies;`
  - `int uevent_set(struct uevent *ev, const char *key, const char *val)` — overwrite existing key's value; else append. Returns 0, or -1 if full (`ev->n >= UE_MAX_KEYS`) on a new key.

- [ ] **Step 1: Write the failing test** — create `tests/test_udev_executor.c`:

```c
#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

int main(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add");
    ue_set(&ev, "DEVPATH", "/devices/x");
    ue_set(&ev, "FOO", "one");

    /* overwrite existing */
    assert(uevent_set(&ev, "FOO", "two") == 0);
    assert(strcmp(uevent_get(&ev, "FOO"), "two") == 0);
    /* append new */
    assert(uevent_set(&ev, "BAR", "baz") == 0);
    assert(strcmp(uevent_get(&ev, "BAR"), "baz") == 0);

    /* a later rule's ENV== sees the updated value */
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(ctx.nsym == 0 && ctx.link_priority == 0 && ctx.escape == 0);
    assert(ctx.nfinal == 0 && ctx.deferred_applies == 0);
    struct rule r;
    ruleset_parse_line("ENV{FOO}==\"two\"", &r);
    assert(rule_match(&r, &ctx) == 1);

    printf("test_udev_executor: uevent_set OK\n");
    printf("test_udev_executor: ALL OK\n");
    return 0;
}
```

- [ ] **Step 2: Wire the Makefile** — add under the ruleset/matcher test lines (~line 103):

```make
	$(CC) $(CFLAGS) tests/test_udev_executor.c -o /tmp/schema-test-executor && /tmp/schema-test-executor
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make test`
Expected: FAIL — compile error (`uevent_set` undefined) or struct field missing.

- [ ] **Step 4: Grow `struct dev_ctx` and add `uevent_set`** in `udev_ruleset.h`.

Add the bound macros near the existing `#define DEVCTX_TAGS_MAX 32`:

```c
#define DEVCTX_SYMLINKS_MAX 32
#define DEVCTX_FINAL_MAX    16
```

Add fields to `struct dev_ctx` (after the existing `matched_parent` member):

```c
    char symlinks[DEVCTX_SYMLINKS_MAX][UE_VAL_MAX];
    int  nsym;
    char mode[8];
    char group[UE_KEY_MAX];
    char owner[UE_KEY_MAX];
    char name[UE_VAL_MAX];
    int  link_priority;
    int  escape;                  /* 0=none, 1=replace */
    char final_keys[DEVCTX_FINAL_MAX][RK_KEY_MAX + RK_SUB_MAX + 2];
    int  nfinal;
    int  last_rule_deferred;
    int  deferred_applies;
```

(`dev_ctx_init` already `memset`s the whole struct — no change needed there.)

Add `uevent_set` (place it near `dev_ctx_init`, above the `ctx` field consumers):

```c
static inline int uevent_set(struct uevent *ev, const char *key, const char *val) {
    for (int i = 0; i < ev->n; i++)
        if (!strcmp(ev->key[i], key)) { safe_copy(ev->val[i], val, UE_VAL_MAX); return 0; }
    if (ev->n >= UE_MAX_KEYS) return -1;
    safe_copy(ev->key[ev->n], key, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], val, UE_VAL_MAX);
    ev->n++;
    return 0;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make test`
Expected: PASS — `test_udev_executor: uevent_set OK` / `ALL OK`, plus every prior test still green, ZERO warnings under c11 and c99.

- [ ] **Step 6: Verify the daemon still builds**

Run: `make schema-udev`
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c Makefile
git commit -m "feat(schema-udev): R3 dev_ctx growth + uevent_set"
```

---

### Task 2: tag ops

**Files:**
- Modify: `udev_ruleset.h` (add `ctx_add_tag`/`ctx_del_tag`/`ctx_clear_tags` after the `struct dev_ctx` helpers)
- Modify: `tests/test_udev_executor.c`

**Interfaces:**
- Consumes: `struct dev_ctx` (`tags`, `ntags`, `DEVCTX_TAGS_MAX`), `safe_copy`.
- Produces:
  - `void ctx_add_tag(struct dev_ctx *ctx, const char *t)` — append if not already present and room remains; else no-op.
  - `void ctx_del_tag(struct dev_ctx *ctx, const char *t)` — remove all occurrences, compacting.
  - `void ctx_clear_tags(struct dev_ctx *ctx)` — `ntags = 0`.

- [ ] **Step 1: Write the failing test** — add before the final `printf`s in `main`:

```c
    struct dev_ctx tc; memset(&tc, 0, sizeof tc);
    ctx_add_tag(&tc, "systemd");
    ctx_add_tag(&tc, "systemd");        /* dedupe */
    ctx_add_tag(&tc, "seat");
    assert(tc.ntags == 2);
    ctx_del_tag(&tc, "systemd");
    assert(tc.ntags == 1 && strcmp(tc.tags[0], "seat") == 0);
    ctx_del_tag(&tc, "nope");           /* absent: no-op */
    assert(tc.ntags == 1);
    ctx_clear_tags(&tc);
    assert(tc.ntags == 0);
    printf("test_udev_executor: tag-ops OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL — `ctx_add_tag` undefined.

- [ ] **Step 3: Implement** in `udev_ruleset.h`:

```c
static inline void ctx_add_tag(struct dev_ctx *ctx, const char *t) {
    for (int i = 0; i < ctx->ntags; i++) if (!strcmp(ctx->tags[i], t)) return;
    if (ctx->ntags >= DEVCTX_TAGS_MAX) return;
    safe_copy(ctx->tags[ctx->ntags++], t, UE_KEY_MAX);
}
static inline void ctx_del_tag(struct dev_ctx *ctx, const char *t) {
    int w = 0;
    for (int i = 0; i < ctx->ntags; i++)
        if (strcmp(ctx->tags[i], t) != 0) {
            if (w != i) safe_copy(ctx->tags[w], ctx->tags[i], UE_KEY_MAX);
            w++;
        }
    ctx->ntags = w;
}
static inline void ctx_clear_tags(struct dev_ctx *ctx) { ctx->ntags = 0; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS — `tag-ops OK`, ZERO warnings.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c
git commit -m "feat(schema-udev): R3 tag ops (add/del/clear)"
```

---

### Task 3: symlink ops + `udev_replace_chars`

**Files:**
- Modify: `udev_ruleset.h` (add `udev_replace_chars` + `ctx_add_symlink`/`ctx_del_symlink`/`ctx_clear_symlinks`)
- Modify: `tests/test_udev_executor.c`

**Interfaces:**
- Consumes: `struct dev_ctx` (`symlinks`, `nsym`, `DEVCTX_SYMLINKS_MAX`), `safe_copy`.
- Produces:
  - `void udev_replace_chars(const char *in, char *out, size_t sz)` — copy `in`→`out`, replacing any byte NOT in the udev devnode whitelist `A-Za-z0-9#+-.:=@_/` with `_`. NUL-terminates within `sz`.
  - `void ctx_add_symlink(struct dev_ctx *ctx, const char *link)` — append verbatim if non-empty, not already present, room remains; else no-op. (Escaping/splitting is the caller's job.)
  - `void ctx_del_symlink(struct dev_ctx *ctx, const char *link)` — remove all occurrences, compacting.
  - `void ctx_clear_symlinks(struct dev_ctx *ctx)` — `nsym = 0`.

- [ ] **Step 1: Write the failing test** — add before the final `printf`s:

```c
    char esc[64];
    udev_replace_chars("a b/c", esc, sizeof esc);   /* space -> _, '/' kept */
    assert(strcmp(esc, "a_b/c") == 0);
    udev_replace_chars("wwn-0x5!bad", esc, sizeof esc); /* '!' -> _ */
    assert(strcmp(esc, "wwn-0x5_bad") == 0);

    struct dev_ctx sc; memset(&sc, 0, sizeof sc);
    ctx_add_symlink(&sc, "disk/by-id/a");
    ctx_add_symlink(&sc, "disk/by-id/a");           /* dedupe */
    ctx_add_symlink(&sc, "disk/by-path/b");
    ctx_add_symlink(&sc, "");                        /* empty: no-op */
    assert(sc.nsym == 2);
    ctx_del_symlink(&sc, "disk/by-id/a");
    assert(sc.nsym == 1 && strcmp(sc.symlinks[0], "disk/by-path/b") == 0);
    ctx_clear_symlinks(&sc);
    assert(sc.nsym == 0);
    printf("test_udev_executor: symlink-ops OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL — `udev_replace_chars` undefined.

- [ ] **Step 3: Implement** in `udev_ruleset.h`:

```c
static inline int udev_wl_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '#' || c == '+' || c == '-' || c == '.' ||
           c == ':' || c == '=' || c == '@' || c == '_' || c == '/';
}
static inline void udev_replace_chars(const char *in, char *out, size_t sz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < sz; p++)
        out[o++] = udev_wl_ok(*p) ? *p : '_';
    if (sz) out[o] = '\0';
}
static inline void ctx_add_symlink(struct dev_ctx *ctx, const char *link) {
    if (!link || !*link) return;
    for (int i = 0; i < ctx->nsym; i++) if (!strcmp(ctx->symlinks[i], link)) return;
    if (ctx->nsym >= DEVCTX_SYMLINKS_MAX) return;
    safe_copy(ctx->symlinks[ctx->nsym++], link, UE_VAL_MAX);
}
static inline void ctx_del_symlink(struct dev_ctx *ctx, const char *link) {
    int w = 0;
    for (int i = 0; i < ctx->nsym; i++)
        if (strcmp(ctx->symlinks[i], link) != 0) {
            if (w != i) safe_copy(ctx->symlinks[w], ctx->symlinks[i], UE_VAL_MAX);
            w++;
        }
    ctx->nsym = w;
}
static inline void ctx_clear_symlinks(struct dev_ctx *ctx) { ctx->nsym = 0; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS — `symlink-ops OK`, ZERO warnings.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c
git commit -m "feat(schema-udev): R3 symlink ops + udev_replace_chars"
```

---

### Task 4: OPTIONS parse + final-lock

**Files:**
- Modify: `udev_ruleset.h` (add `apply_options`, `ctx_key_final`, `ctx_lock_final`)
- Modify: `tests/test_udev_executor.c`

**Interfaces:**
- Consumes: `struct dev_ctx` (`link_priority`, `escape`, `final_keys`, `nfinal`, `DEVCTX_FINAL_MAX`), `struct rule_clause` (`key`, `subkey`), `safe_copy`.
- Produces:
  - `void apply_options(struct dev_ctx *ctx, const char *val)` — split `val` on `,` and whitespace; `link_priority=N` → `ctx->link_priority = atoi(N)`; `string_escape=replace` → `ctx->escape = 1`; `string_escape=none` → `ctx->escape = 0`; any other token ignored (no-op, no error).
  - `int ctx_key_final(const struct dev_ctx *ctx, const struct rule_clause *c)` — build `"KEY{sub}"` (or `"KEY"` when `subkey` empty); return 1 if that token is in `final_keys`, else 0.
  - `void ctx_lock_final(struct dev_ctx *ctx, const struct rule_clause *c)` — append the token if absent and room remains.

- [ ] **Step 1: Write the failing test** — add before the final `printf`s:

```c
    struct dev_ctx oc; memset(&oc, 0, sizeof oc);
    apply_options(&oc, "link_priority=10");
    assert(oc.link_priority == 10);
    apply_options(&oc, "string_escape=replace");
    assert(oc.escape == 1);
    apply_options(&oc, "string_escape=none");
    assert(oc.escape == 0);
    apply_options(&oc, "static_node=foo");           /* no-op, no crash */
    apply_options(&oc, "db_persist, link_priority=-5"); /* mixed list */
    assert(oc.link_priority == -5);

    struct rule_clause c1; memset(&c1, 0, sizeof c1);
    safe_copy(c1.key, "NAME", sizeof c1.key);
    assert(ctx_key_final(&oc, &c1) == 0);
    ctx_lock_final(&oc, &c1);
    assert(ctx_key_final(&oc, &c1) == 1);
    ctx_lock_final(&oc, &c1);                          /* idempotent */
    assert(oc.nfinal == 1);
    struct rule_clause c2; memset(&c2, 0, sizeof c2);
    safe_copy(c2.key, "ENV", sizeof c2.key);
    safe_copy(c2.subkey, "FOO", sizeof c2.subkey);
    assert(ctx_key_final(&oc, &c2) == 0);              /* ENV{FOO} distinct from NAME */
    printf("test_udev_executor: options+final OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL — `apply_options` undefined.

- [ ] **Step 3: Implement** in `udev_ruleset.h`:

```c
static inline void ctx_final_token(const struct rule_clause *c, char *out, size_t sz) {
    if (c->subkey[0]) snprintf(out, sz, "%s{%s}", c->key, c->subkey);
    else              snprintf(out, sz, "%s", c->key);
}
static inline int ctx_key_final(const struct dev_ctx *ctx, const struct rule_clause *c) {
    char tok[RK_KEY_MAX + RK_SUB_MAX + 2]; ctx_final_token(c, tok, sizeof tok);
    for (int i = 0; i < ctx->nfinal; i++) if (!strcmp(ctx->final_keys[i], tok)) return 1;
    return 0;
}
static inline void ctx_lock_final(struct dev_ctx *ctx, const struct rule_clause *c) {
    if (ctx_key_final(ctx, c) || ctx->nfinal >= DEVCTX_FINAL_MAX) return;
    ctx_final_token(c, ctx->final_keys[ctx->nfinal++], RK_KEY_MAX + RK_SUB_MAX + 2);
}
static inline void apply_options(struct dev_ctx *ctx, const char *val) {
    const char *p = val;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - s);
        char tok[128];
        if (len == 0 || len >= sizeof tok) continue;
        memcpy(tok, s, len); tok[len] = '\0';
        if      (!strncmp(tok, "link_priority=", 14)) ctx->link_priority = atoi(tok + 14);
        else if (!strcmp(tok, "string_escape=replace")) ctx->escape = 1;
        else if (!strcmp(tok, "string_escape=none"))    ctx->escape = 0;
        /* static_node=, watch, nowatch, db_persist, ... : tracked no-ops */
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS — `options+final OK`, ZERO warnings.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c
git commit -m "feat(schema-udev): R3 OPTIONS parse + final-lock"
```

---

### Task 5: `apply_rule` — one rule's assignments

**Files:**
- Modify: `udev_ruleset.h` (add `apply_rule` after the helpers, before `ruleset_apply`)
- Modify: `tests/test_udev_executor.c`

**Interfaces:**
- Consumes: `struct rule`/`struct rule_clause` (`key`, `subkey`, `op`, `val`, `nclause`), `enum rule_op` (`OP_ASSIGN`, `OP_ASSIGN_ADD`, `OP_ASSIGN_SUB`, `OP_ASSIGN_FINAL`, and the match ops), `rk_is_match_op`, `ruleset_subst`, `uevent_set`, all `ctx_*` helpers, `udev_replace_chars`, `apply_options`.
- Produces:
  - `const char *apply_rule(const struct rule *r, struct dev_ctx *ctx)` — apply every assignment clause of `r` in order. Returns a pointer to the GOTO target label string (the clause's `val`, which lives in the ruleset) when a `GOTO` is executed; otherwise `NULL`. `LABEL` clauses are no-ops. Match-op clauses are skipped. A clause whose key is final-locked (`ctx_key_final`) is skipped. `OP_ASSIGN_FINAL` (`:=`) applies then locks the key. Values (except GOTO/LABEL targets) pass through `ruleset_subst`.

- [ ] **Step 1: Write the failing test** — add before the final `printf`s:

```c
    /* build a dev_ctx over a minimal device */
    struct uevent ae; memset(&ae, 0, sizeof ae);
    ue_set(&ae, "ACTION", "add"); ue_set(&ae, "DEVPATH", "/devices/z");
    struct dev_ctx ac; assert(dev_ctx_init(&ac, &ae, "/sys") == 0);

    struct rule ar;
    ruleset_parse_line("ENV{MYK}=\"v1\", TAG+=\"uaccess\", MODE=\"0660\", GROUP=\"plugdev\"", &ar);
    assert(apply_rule(&ar, &ac) == NULL);
    assert(strcmp(uevent_get(&ae, "MYK"), "v1") == 0);
    assert(ac.ntags == 1 && strcmp(ac.tags[0], "uaccess") == 0);
    assert(strcmp(ac.mode, "0660") == 0 && strcmp(ac.group, "plugdev") == 0);

    /* SYMLINK+= with a space-separated list -> two links */
    ruleset_parse_line("SYMLINK+=\"disk/by-id/x disk/by-path/y\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.nsym == 2);

    /* string_escape=replace: whitespace escaped -> a single link */
    ac.escape = 1;
    ruleset_parse_line("SYMLINK+=\"has space\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.nsym == 3 && strcmp(ac.symlinks[2], "has_space") == 0);
    ac.escape = 0;

    /* TAG-= removes */
    ruleset_parse_line("TAG-=\"uaccess\"", &ar);
    apply_rule(&ar, &ac);
    assert(ac.ntags == 0);

    /* := locks the key: a later = does not override */
    ruleset_parse_line("NAME:=\"locked\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(ac.name, "locked") == 0);
    ruleset_parse_line("NAME=\"other\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(ac.name, "locked") == 0);

    /* GOTO returns the target label */
    ruleset_parse_line("GOTO=\"end_here\"", &ar);
    const char *g = apply_rule(&ar, &ac);
    assert(g != NULL && strcmp(g, "end_here") == 0);

    /* substitution runs on values */
    ruleset_parse_line("ENV{KN}=\"%k\"", &ar);
    apply_rule(&ar, &ac);
    assert(strcmp(uevent_get(&ae, "KN"), "z") == 0);
    printf("test_udev_executor: apply-rule OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL — `apply_rule` undefined.

- [ ] **Step 3: Implement** in `udev_ruleset.h`:

```c
/* add each whitespace-separated symlink token (escaped) from a SYMLINK value */
static inline void apply_symlink_value(struct dev_ctx *ctx, const char *v, enum rule_op op) {
    if (op == OP_ASSIGN || op == OP_ASSIGN_FINAL) ctx_clear_symlinks(ctx);
    if (ctx->escape) {                    /* whole value -> one escaped link */
        char e[UE_VAL_MAX]; udev_replace_chars(v, e, sizeof e);
        if (op == OP_ASSIGN_SUB) ctx_del_symlink(ctx, e); else ctx_add_symlink(ctx, e);
        return;
    }
    const char *p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - s);
        if (!len) continue;
        char raw[UE_VAL_MAX], e[UE_VAL_MAX];
        if (len >= sizeof raw) len = sizeof raw - 1;
        memcpy(raw, s, len); raw[len] = '\0';
        udev_replace_chars(raw, e, sizeof e);
        if (op == OP_ASSIGN_SUB) ctx_del_symlink(ctx, e); else ctx_add_symlink(ctx, e);
    }
}

static inline const char *apply_rule(const struct rule *r, struct dev_ctx *ctx) {
    for (int i = 0; i < r->nclause; i++) {
        const struct rule_clause *c = &r->clause[i];
        if (rk_is_match_op(c->op)) continue;

        if (!strcmp(c->key, "GOTO"))  return c->val;
        if (!strcmp(c->key, "LABEL")) continue;

        if (ctx_key_final(ctx, c)) continue;

        char sv[UE_VAL_MAX];
        ruleset_subst(c->val, ctx, sv, sizeof sv);

        if (!strcmp(c->key, "ENV")) {
            uevent_set(ctx->ev, c->subkey, sv);
        } else if (!strcmp(c->key, "TAG")) {
            if (c->op == OP_ASSIGN_SUB) ctx_del_tag(ctx, sv);
            else { if (c->op == OP_ASSIGN || c->op == OP_ASSIGN_FINAL) ctx_clear_tags(ctx);
                   ctx_add_tag(ctx, sv); }
        } else if (!strcmp(c->key, "SYMLINK")) {
            apply_symlink_value(ctx, sv, c->op);
        } else if (!strcmp(c->key, "OPTIONS")) {
            apply_options(ctx, sv);
        } else if (!strcmp(c->key, "MODE")) {
            safe_copy(ctx->mode, sv, sizeof ctx->mode);
        } else if (!strcmp(c->key, "GROUP")) {
            safe_copy(ctx->group, sv, sizeof ctx->group);
        } else if (!strcmp(c->key, "OWNER")) {
            safe_copy(ctx->owner, sv, sizeof ctx->owner);
        } else if (!strcmp(c->key, "NAME")) {
            safe_copy(ctx->name, sv, sizeof ctx->name);
        }
        /* IMPORT / RUN / other: R4 — ignored here */

        if (c->op == OP_ASSIGN_FINAL) ctx_lock_final(ctx, c);
    }
    return NULL;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: PASS — `apply-rule OK`, ZERO warnings under c11 AND c99.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c
git commit -m "feat(schema-udev): R3 apply_rule (assignment clauses + GOTO signal)"
```

---

### Task 6: `ruleset_apply` driver + deferred flag + live smoke

**Files:**
- Modify: `udev_ruleset.h` (edit `rule_match` to maintain `last_rule_deferred`; add `ruleset_apply` at the end, before `#endif`)
- Modify: `tests/test_udev_executor.c`

**Interfaces:**
- Consumes: `struct ruleset` (`rules`, `n`), `rule`, `apply_rule`, `rule_match`, `struct rule_clause`, `ruleset_parse_line`, `ruleset_append`, `ruleset_load_dirs`.
- Produces:
  - `rule_match` now sets `ctx->last_rule_deferred = 0` at entry and `= 1` whenever it skips an unknown (R4-deferred) match key (the existing `TEST`/`PROGRAM`/`RESULT` fall-through path). No signature change.
  - `int ruleset_apply(const struct ruleset *rs, struct dev_ctx *ctx)` — walk `rs->rules[0..n)` in order. For each rule where `rule_match==1`: if `ctx->last_rule_deferred` bump `ctx->deferred_applies`, then `apply_rule`; if it returned a GOTO label, jump to the first later rule (index ≥ current, matching udev forward-only GOTO) carrying a `LABEL=` clause with that value — if none found, stop. Returns 0.

- [ ] **Step 1: Write the failing test** — add before the final `printf`s:

```c
    /* helper: append a parsed line to a ruleset */
    #define ADD(RS, LINE) do { struct rule _r; \
        assert(ruleset_parse_line((LINE), &_r) > 0); \
        assert(ruleset_append((RS), &_r) == 0); } while (0)

    /* ENV set by an early rule is visible to a later rule's ENV== match */
    struct uevent de; memset(&de, 0, sizeof de);
    ue_set(&de, "ACTION", "add"); ue_set(&de, "DEVPATH", "/devices/w");
    struct dev_ctx dc; assert(dev_ctx_init(&dc, &de, "/sys") == 0);
    struct ruleset rs1 = {0};
    ADD(&rs1, "ACTION==\"add\", ENV{PHASE}=\"two\"");
    ADD(&rs1, "ENV{PHASE}==\"two\", TAG+=\"reached\"");
    assert(ruleset_apply(&rs1, &dc) == 0);
    assert(dc.ntags == 1 && strcmp(dc.tags[0], "reached") == 0);
    free(rs1.rules);

    /* GOTO skips the intervening rule's assignment */
    struct dev_ctx gc; memset(&gc, 0, sizeof gc);
    struct uevent ge; memset(&ge, 0, sizeof ge);
    ue_set(&ge, "ACTION", "add"); ue_set(&ge, "DEVPATH", "/devices/g");
    assert(dev_ctx_init(&gc, &ge, "/sys") == 0);
    struct ruleset rs2 = {0};
    ADD(&rs2, "ACTION==\"add\", GOTO=\"skip\"");
    ADD(&rs2, "TAG+=\"should_not_appear\"");
    ADD(&rs2, "LABEL=\"skip\"");
    ADD(&rs2, "TAG+=\"after_label\"");
    assert(ruleset_apply(&rs2, &gc) == 0);
    assert(gc.ntags == 1 && strcmp(gc.tags[0], "after_label") == 0);
    free(rs2.rules);

    /* deferred gate: a TEST== rule still applies (superset) and bumps the counter */
    struct dev_ctx fc; memset(&fc, 0, sizeof fc);
    struct uevent fe; memset(&fe, 0, sizeof fe);
    ue_set(&fe, "ACTION", "add"); ue_set(&fe, "DEVPATH", "/devices/f");
    assert(dev_ctx_init(&fc, &fe, "/sys") == 0);
    struct ruleset rs3 = {0};
    ADD(&rs3, "ACTION==\"add\", TEST==\"/nonexistent/path\", TAG+=\"superset\"");
    assert(ruleset_apply(&rs3, &fc) == 0);
    assert(fc.ntags == 1 && strcmp(fc.tags[0], "superset") == 0);
    assert(fc.deferred_applies > 0);
    free(rs3.rules);

    /* GOTO to a missing label stops cleanly (no crash, no later apply) */
    struct dev_ctx mc; memset(&mc, 0, sizeof mc);
    struct uevent me; memset(&me, 0, sizeof me);
    ue_set(&me, "ACTION", "add"); ue_set(&me, "DEVPATH", "/devices/m");
    assert(dev_ctx_init(&mc, &me, "/sys") == 0);
    struct ruleset rs4 = {0};
    ADD(&rs4, "GOTO=\"nowhere\"");
    ADD(&rs4, "TAG+=\"unreached\"");
    assert(ruleset_apply(&rs4, &mc) == 0);
    assert(mc.ntags == 0);
    free(rs4.rules);
    printf("test_udev_executor: driver OK\n");

    /* live smoke: apply the whole installed ruleset to a real device, no crash */
    if (access("/sys/block/sda", F_OK) == 0) {
        char lnk[PATH_MAX]; ssize_t ln = readlink("/sys/block/sda", lnk, sizeof lnk - 1);
        assert(ln > 0); lnk[ln] = '\0';
        const char *dp = strstr(lnk, "/devices/"); assert(dp);
        struct uevent le; memset(&le, 0, sizeof le);
        ue_set(&le, "ACTION", "add"); ue_set(&le, "DEVPATH", dp);
        ue_set(&le, "SUBSYSTEM", "block");
        struct dev_ctx lc; assert(dev_ctx_init(&lc, &le, "/sys") == 0);
        const char *real[] = { "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };
        struct ruleset live = {0};
        assert(ruleset_load_dirs(real, 3, &live) == 0);
        assert(ruleset_apply(&live, &lc) == 0);   /* must not crash */
        assert(lc.ntags >= 0);                    /* sda typically gets "systemd" */
        free(live.rules);
        printf("test_udev_executor: live-smoke OK (sda tags=%d symlinks=%d)\n", lc.ntags, lc.nsym);
    }
    #undef ADD
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: FAIL — `ruleset_apply` undefined.

- [ ] **Step 3: Edit `rule_match`** to maintain the deferred flag. At the top of `rule_match` (before the loop) add:

```c
    ctx->last_rule_deferred = 0;
```

In the fall-through for an unknown match key (the final `continue;` at the end of the loop body — the `/* unknown match key (TEST/PROGRAM): deferred to R4 */` branch), set the flag before continuing:

```c
        ctx->last_rule_deferred = 1;   /* a deferred conditional was skipped */
        continue;   /* unknown match key (TEST/PROGRAM): deferred to R4 */
```

- [ ] **Step 4: Add `ruleset_apply`** at the end of `udev_ruleset.h`, before `#endif`:

```c
/* Find the first rule at index >= from carrying LABEL=="label"; -1 if none. */
static inline int ruleset_find_label(const struct ruleset *rs, int from, const char *label) {
    for (int i = from; i < rs->n; i++)
        for (int k = 0; k < rs->rules[i].nclause; k++) {
            const struct rule_clause *c = &rs->rules[i].clause[k];
            if (!strcmp(c->key, "LABEL") && !strcmp(c->val, label)) return i;
        }
    return -1;
}

static inline int ruleset_apply(const struct ruleset *rs, struct dev_ctx *ctx) {
    for (int i = 0; i < rs->n; ) {
        if (!rule_match(&rs->rules[i], ctx)) { i++; continue; }
        if (ctx->last_rule_deferred) ctx->deferred_applies++;
        const char *goto_label = apply_rule(&rs->rules[i], ctx);
        if (goto_label) {
            int t = ruleset_find_label(rs, i + 1, goto_label);
            if (t < 0) break;      /* forward label not found: stop */
            i = t;
        } else {
            i++;
        }
    }
    return 0;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make test`
Expected: PASS — `driver OK` and `live-smoke OK`, every prior test green, ZERO warnings under c11 AND c99.

- [ ] **Step 6: Verify the daemon still builds**

Run: `make schema-udev`
Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_executor.c
git commit -m "feat(schema-udev): R3 ruleset_apply driver + deferred-gate reporting + live smoke"
```

---

## Self-Review

**Spec coverage:**
- ENV/TAG/SYMLINK/OPTIONS/MODE/GROUP/OWNER/NAME assignments → Task 5 (`apply_rule`). ✅
- GOTO/LABEL forward control flow → Task 6 (`ruleset_apply` + `ruleset_find_label`). ✅
- `dev_ctx` growth (symlinks/mode/group/owner/name/link_priority/escape/final/deferred) → Task 1. ✅
- ENV mutates `ev` mid-run, later `ENV==` sees it → Task 1 (unit) + Task 6 (driver-level). ✅
- Deferred-gate superset apply + reported counter (decision 1) → Task 6 (`last_rule_deferred`/`deferred_applies`). ✅
- `:=` as assign + final-lock (decision 2) → Task 4 (`ctx_*_final`) + Task 5 (apply then lock). ✅
- OPTIONS link_priority/string_escape honored, others tracked no-ops (decision 2) → Task 4. ✅
- `string_escape` symlink handling → Task 3 (`udev_replace_chars`) + Task 5 (`apply_symlink_value`). ✅
- Live shadow smoke on a real device → Task 6. ✅
- Verify: `make test` c11+c99 zero warnings, daemon builds, live box untouched → every task's verify steps + Global Constraints. ✅

**Placeholder scan:** No TBD/TODO/"handle edge cases"/vague steps — every code step has literal code. ✅

**Type consistency:** `apply_rule` returns `const char *` (GOTO label / NULL) consumed by `ruleset_apply` — consistent. `ctx_key_final`/`ctx_lock_final` take `const struct rule_clause *` — consistent across Tasks 4/5. `apply_symlink_value` (introduced in Task 5) takes `enum rule_op` — matches `c->op`. `uevent_set(ev,key,val)` signature identical in Task 1 def and Task 5 use. Field names (`nsym`, `link_priority`, `escape`, `deferred_applies`, `last_rule_deferred`) identical between Task 1 struct and all consumers. ✅
