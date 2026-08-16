# schema-udev Rule Interpreter — R1 Parser/Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parse the installed udev `.rules` set into an in-memory ruleset — the R1 slice of the rule interpreter (spec `2026-08-10-schema-udev-rule-interpreter-design.md`).

**Architecture:** A new header-only unit `udev_ruleset.h` (matching the codebase's `static inline` header style) provides a clause tokenizer, a rule-line parser, a continuation/comment-aware file loader, and a precedence-merging directory loader. R1 only parses — no matching or execution. Output is a `struct ruleset` consumed by R2 (matcher).

**Tech Stack:** C11, `-Wall -Wextra -D_GNU_SOURCE`, header-only inline functions, assert-based tests in `tests/`, wired into `make test`. No external libraries.

## Global Constraints

- Header-only `static inline`, no new `.c` in the build except the test file — matches `udev_db.h`, `udev_rules.h`, `schema-udev.h`.
- Compile clean under `-std=c11 -Wall -Wextra -D_GNU_SOURCE`.
- No heap where a fixed bound suffices; where growth is needed use `realloc` with explicit `cap`/`n` (ruleset array only).
- Constants: `RK_KEY_MAX 32`, `RK_SUB_MAX 128`, `RK_VAL_MAX 512`, `RULE_MAX_CLAUSES 32`.
- Operators enum: `OP_MATCH_EQ` (`==`), `OP_MATCH_NE` (`!=`), `OP_ASSIGN` (`=`), `OP_ASSIGN_ADD` (`+=`), `OP_ASSIGN_SUB` (`-=`), `OP_ASSIGN_FINAL` (`:=`).
- Use `safe_copy` (from `schema-udev.h`) for all bounded string copies.

---

## File Structure

- Create: `udev_ruleset.h` — data structures + parse/load functions.
- Create: `tests/test_udev_ruleset.c` — TDD test binary.
- Modify: `Makefile` — add the test to the `test:` target.

Data structures (defined in Task 1, referenced by all later tasks):

```c
enum rule_op { OP_MATCH_EQ, OP_MATCH_NE, OP_ASSIGN, OP_ASSIGN_ADD, OP_ASSIGN_SUB, OP_ASSIGN_FINAL };

struct rule_clause {
    char key[RK_KEY_MAX];      /* e.g. "ACTION", "ATTR", "GOTO" */
    char subkey[RK_SUB_MAX];   /* {..} contents, "" if none */
    enum rule_op op;
    char val[RK_VAL_MAX];      /* unquoted value */
};

struct rule {
    struct rule_clause clause[RULE_MAX_CLAUSES];
    int nclause;
};

struct ruleset {
    struct rule *rules;        /* heap array */
    int n;
    int cap;
};
```

---

### Task 1: Clause tokenizer

**Files:**
- Create: `udev_ruleset.h`
- Test: `tests/test_udev_ruleset.c`

**Interfaces:**
- Produces: `int ruleset_parse_clause(const char *s, struct rule_clause *out)` — parse a single clause like `ATTR{parameters/x}=="0"` or `GOTO="end"`. Returns `0` on success, `-1` on malformed. Fills `out->key`, `out->subkey` (`""` if absent), `out->op`, `out->val` (quotes stripped).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_udev_ruleset.c`:

```c
#include "../udev_ruleset.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct rule_clause c;

    /* match, no subkey */
    assert(ruleset_parse_clause("ACTION==\"add\"", &c) == 0);
    assert(strcmp(c.key, "ACTION") == 0 && c.subkey[0] == '\0' &&
           c.op == OP_MATCH_EQ && strcmp(c.val, "add") == 0);

    /* not-equal match */
    assert(ruleset_parse_clause("ACTION!=\"remove\"", &c) == 0);
    assert(c.op == OP_MATCH_NE && strcmp(c.val, "remove") == 0);

    /* subkey with slash/glob */
    assert(ruleset_parse_clause("ATTR{parameters/events_dfl_poll_msecs}==\"0\"", &c) == 0);
    assert(strcmp(c.key, "ATTR") == 0 &&
           strcmp(c.subkey, "parameters/events_dfl_poll_msecs") == 0 &&
           c.op == OP_MATCH_EQ && strcmp(c.val, "0") == 0);

    /* each assignment operator */
    assert(ruleset_parse_clause("OPTIONS+=\"watch\"", &c) == 0 && c.op == OP_ASSIGN_ADD);
    assert(ruleset_parse_clause("TAG-=\"seat\"", &c) == 0 && c.op == OP_ASSIGN_SUB);
    assert(ruleset_parse_clause("MODE=\"660\"", &c) == 0 && c.op == OP_ASSIGN);
    assert(ruleset_parse_clause("ENV{ID_X}:=\"1\"", &c) == 0 &&
           c.op == OP_ASSIGN_FINAL && strcmp(c.subkey, "ID_X") == 0);

    /* value containing | alternation and glob preserved verbatim */
    assert(ruleset_parse_clause("KERNEL==\"sd*|vd*\"", &c) == 0 &&
           strcmp(c.val, "sd*|vd*") == 0);

    /* malformed: no operator */
    assert(ruleset_parse_clause("ACTION add", &c) == -1);

    printf("test_udev_ruleset: clause OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset`
Expected: FAIL — `udev_ruleset.h` does not exist / `ruleset_parse_clause` undefined.

- [ ] **Step 3: Write minimal implementation**

Create `udev_ruleset.h`:

```c
#ifndef UDEV_RULESET_H
#define UDEV_RULESET_H

#include "schema-udev.h"   /* safe_copy */
#include <string.h>
#include <stdlib.h>

#define RK_KEY_MAX 32
#define RK_SUB_MAX 128
#define RK_VAL_MAX 512
#define RULE_MAX_CLAUSES 32

enum rule_op { OP_MATCH_EQ, OP_MATCH_NE, OP_ASSIGN, OP_ASSIGN_ADD, OP_ASSIGN_SUB, OP_ASSIGN_FINAL };

struct rule_clause {
    char key[RK_KEY_MAX];
    char subkey[RK_SUB_MAX];
    enum rule_op op;
    char val[RK_VAL_MAX];
};

struct rule { struct rule_clause clause[RULE_MAX_CLAUSES]; int nclause; };
struct ruleset { struct rule *rules; int n; int cap; };

/* Parse "KEY{sub}OP\"val\"" -> clause. Returns 0 / -1. */
static inline int ruleset_parse_clause(const char *s, struct rule_clause *out) {
    memset(out, 0, sizeof *out);
    while (*s == ' ' || *s == '\t') s++;
    const char *p = s;
    /* key = leading [A-Z_] run */
    while ((*p >= 'A' && *p <= 'Z') || *p == '_') p++;
    size_t klen = (size_t)(p - s);
    if (klen == 0 || klen >= RK_KEY_MAX) return -1;
    memcpy(out->key, s, klen); out->key[klen] = '\0';
    /* optional {subkey} */
    if (*p == '{') {
        const char *e = strchr(p, '}');
        if (!e) return -1;
        size_t sl = (size_t)(e - (p + 1));
        if (sl >= RK_SUB_MAX) return -1;
        memcpy(out->subkey, p + 1, sl); out->subkey[sl] = '\0';
        p = e + 1;
    }
    /* operator */
    if      (p[0] == '=' && p[1] == '=') { out->op = OP_MATCH_EQ;     p += 2; }
    else if (p[0] == '!' && p[1] == '=') { out->op = OP_MATCH_NE;     p += 2; }
    else if (p[0] == '+' && p[1] == '=') { out->op = OP_ASSIGN_ADD;   p += 2; }
    else if (p[0] == '-' && p[1] == '=') { out->op = OP_ASSIGN_SUB;   p += 2; }
    else if (p[0] == ':' && p[1] == '=') { out->op = OP_ASSIGN_FINAL; p += 2; }
    else if (p[0] == '=')                { out->op = OP_ASSIGN;       p += 1; }
    else return -1;
    /* quoted value */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t vl = (size_t)(e - p);
    if (vl >= RK_VAL_MAX) return -1;
    memcpy(out->val, p, vl); out->val[vl] = '\0';
    return 0;
}

#endif /* UDEV_RULESET_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset && /tmp/t-ruleset`
Expected: `test_udev_ruleset: clause OK`

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_ruleset.c
git commit -m "feat(schema-udev): R1 clause tokenizer (udev_ruleset.h)"
```

---

### Task 2: Rule-line parser (comma-split respecting quotes)

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_ruleset.c`

**Interfaces:**
- Consumes: `ruleset_parse_clause`.
- Produces: `int ruleset_parse_line(const char *line, struct rule *out)` — split a logical rule line on top-level commas (commas inside `"..."` are literal), parse each segment as a clause. Returns clause count `>0`, or `-1` on malformed, or `0` for an empty/whitespace line.

- [ ] **Step 1: Write the failing test**

Add before the final `printf` in `main`:

```c
    struct rule r;
    int nc = ruleset_parse_line(
        "ACTION==\"change\", SUBSYSTEM==\"block\", KERNEL==\"loop*\", GROUP=\"disk\", MODE=\"660\"", &r);
    assert(nc == 5 && r.nclause == 5);
    assert(strcmp(r.clause[0].key, "ACTION") == 0 && r.clause[0].op == OP_MATCH_EQ);
    assert(strcmp(r.clause[3].key, "GROUP") == 0 && strcmp(r.clause[3].val, "disk") == 0);
    assert(strcmp(r.clause[4].key, "MODE") == 0 && strcmp(r.clause[4].val, "660") == 0);

    /* GOTO / LABEL are single-clause lines */
    assert(ruleset_parse_line("ACTION==\"remove\", GOTO=\"uaccess_end\"", &r) == 2);
    assert(strcmp(r.clause[1].key, "GOTO") == 0 && strcmp(r.clause[1].val, "uaccess_end") == 0);
    assert(ruleset_parse_line("LABEL=\"uaccess_end\"", &r) == 1);

    /* blank line -> 0 clauses */
    assert(ruleset_parse_line("   ", &r) == 0);

    printf("test_udev_ruleset: line OK\n");
```

Change the earlier `printf("test_udev_ruleset: clause OK\n");` line to remain (both prints are fine), or leave one final print. Keep the clause asserts above it.

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset`
Expected: FAIL — `ruleset_parse_line` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` before `#endif`:

```c
/* Split on top-level commas (quotes are literal), parse each as a clause. */
static inline int ruleset_parse_line(const char *line, struct rule *out) {
    out->nclause = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        /* find end of this clause: next top-level comma */
        const char *seg = p;
        int inq = 0;
        while (*p && !(*p == ',' && !inq)) { if (*p == '"') inq = !inq; p++; }
        size_t seglen = (size_t)(p - seg);
        char buf[RK_KEY_MAX + RK_SUB_MAX + RK_VAL_MAX + 8];
        if (seglen == 0 || seglen >= sizeof buf) { if (*p == ',') p++; continue; }
        memcpy(buf, seg, seglen); buf[seglen] = '\0';
        if (out->nclause >= RULE_MAX_CLAUSES) return -1;
        if (ruleset_parse_clause(buf, &out->clause[out->nclause]) != 0) return -1;
        out->nclause++;
        if (*p == ',') p++;
    }
    return out->nclause;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset && /tmp/t-ruleset`
Expected: both `clause OK` and `line OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_ruleset.c
git commit -m "feat(schema-udev): R1 rule-line parser (comma-split, quote-aware)"
```

---

### Task 3: File loader (continuation join, comment/blank skip)

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_ruleset.c`

**Interfaces:**
- Consumes: `ruleset_parse_line`.
- Produces:
  - `int ruleset_append(struct ruleset *rs, const struct rule *r)` — grow-and-append (realloc by doubling from cap 0). Returns 0 / -1.
  - `int ruleset_load_file(const char *path, struct ruleset *rs)` — read file, join trailing-`\` continuations, skip `#`/blank lines, parse each logical line, append. Returns 0 / -1. Malformed individual lines are skipped (not fatal), matching udev's leniency.

- [ ] **Step 1: Write the failing test**

Add before the final print:

```c
    /* file with a comment, a continuation, and two rules */
    char tmpl[] = "/tmp/schema-ruleset-XXXXXX";
    int fd = mkstemp(tmpl); assert(fd >= 0);
    const char *content =
        "# a comment\n"
        "ACTION!=\"remove\", SUBSYSTEM==\"block\", \\\n"
        "  KERNEL==\"sd*|vd*\", OPTIONS+=\"watch\"\n"
        "\n"
        "LABEL=\"end\"\n";
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);

    struct ruleset rs = {0};
    assert(ruleset_load_file(tmpl, &rs) == 0);
    assert(rs.n == 2);
    assert(rs.rules[0].nclause == 4);
    assert(strcmp(rs.rules[0].clause[2].key, "KERNEL") == 0 &&
           strcmp(rs.rules[0].clause[2].val, "sd*|vd*") == 0);
    assert(strcmp(rs.rules[1].clause[0].key, "LABEL") == 0);
    free(rs.rules);
    unlink(tmpl);

    printf("test_udev_ruleset: file OK\n");
```

Add `#include <unistd.h>` and `#include <stdlib.h>` to the test includes.

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset`
Expected: FAIL — `ruleset_load_file` / `ruleset_append` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` (needs `#include <stdio.h>` at top):

```c
static inline int ruleset_append(struct ruleset *rs, const struct rule *r) {
    if (rs->n >= rs->cap) {
        int ncap = rs->cap ? rs->cap * 2 : 64;
        struct rule *nr = realloc(rs->rules, (size_t)ncap * sizeof *nr);
        if (!nr) return -1;
        rs->rules = nr; rs->cap = ncap;
    }
    rs->rules[rs->n++] = *r;
    return 0;
}

static inline int ruleset_load_file(const char *path, struct ruleset *rs) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char logical[4096]; size_t llen = 0; int cont = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        const char *s = line;
        if (!cont) { while (*s == ' ' || *s == '\t') s++; }
        cont = 0;
        if (len && line[len-1] == '\\') { line[--len] = '\0'; cont = 1;
            /* re-trim s length after removing backslash */
            len = strlen(s); }
        else len = strlen(s);
        if (llen + strlen(s) < sizeof logical) { memcpy(logical + llen, s, strlen(s)); llen += strlen(s); logical[llen] = '\0'; }
        if (cont) continue;
        if (llen && logical[0] != '#') {
            struct rule r;
            if (ruleset_parse_line(logical, &r) > 0) ruleset_append(rs, &r);
        }
        llen = 0; logical[0] = '\0';
    }
    fclose(f);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset && /tmp/t-ruleset`
Expected: `file OK` printed, no assert failures.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_ruleset.c
git commit -m "feat(schema-udev): R1 file loader (continuation join, comment skip)"
```

---

### Task 4: Directory loader with udev precedence

**Files:**
- Modify: `udev_ruleset.h`
- Test: `tests/test_udev_ruleset.c`

**Interfaces:**
- Consumes: `ruleset_load_file`.
- Produces: `int ruleset_load_dirs(const char *const *dirs, int ndirs, struct ruleset *rs)` — collect `*.rules` filenames across all `dirs`; for each unique basename, the highest-precedence dir (later in the `dirs` array wins) provides the file; process unique basenames in lexical order, loading each into `rs`. Returns 0 / -1. Caller passes dirs low→high precedence: `{"/usr/lib/udev/rules.d","/run/udev/rules.d","/etc/udev/rules.d"}`.

- [ ] **Step 1: Write the failing test**

Add before the final print:

```c
    /* two dirs; /etc overrides /usr/lib for same basename; lexical order across names */
    char d1[] = "/tmp/schema-rd1-XXXXXX"; assert(mkdtemp(d1));
    char d2[] = "/tmp/schema-rd2-XXXXXX"; assert(mkdtemp(d2));
    char pa[256], pb[256], pc[256];
    snprintf(pa, sizeof pa, "%s/50-a.rules", d1);
    snprintf(pb, sizeof pb, "%s/90-z.rules", d1);
    snprintf(pc, sizeof pc, "%s/50-a.rules", d2);   /* overrides d1's 50-a */
    FILE *fa = fopen(pa, "w"); fputs("KERNEL==\"fromd1\"\n", fa); fclose(fa);
    FILE *fb = fopen(pb, "w"); fputs("KERNEL==\"zlast\"\n", fb); fclose(fb);
    FILE *fc = fopen(pc, "w"); fputs("KERNEL==\"fromd2\"\n", fc); fclose(fc);

    const char *dirs[] = { d1, d2 };   /* d2 = higher precedence */
    struct ruleset rs2 = {0};
    assert(ruleset_load_dirs(dirs, 2, &rs2) == 0);
    assert(rs2.n == 2);
    /* 50-a comes before 90-z lexically; 50-a resolved from d2 */
    assert(strcmp(rs2.rules[0].clause[0].val, "fromd2") == 0);
    assert(strcmp(rs2.rules[1].clause[0].val, "zlast") == 0);
    free(rs2.rules);
    unlink(pa); unlink(pb); unlink(pc); rmdir(d1); rmdir(d2);

    printf("test_udev_ruleset: dirs OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset`
Expected: FAIL — `ruleset_load_dirs` undefined.

- [ ] **Step 3: Write minimal implementation**

Add to `udev_ruleset.h` (needs `#include <dirent.h>`):

```c
/* Resolve *.rules basenames across dirs (later dir wins), process in lexical
 * basename order. Bounded to 1024 unique rule files. */
static inline int ruleset_load_dirs(const char *const *dirs, int ndirs, struct ruleset *rs) {
    char names[1024][64];
    char owner[1024][256];   /* full path of winning dir for that basename */
    int nn = 0;
    for (int di = 0; di < ndirs; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 7 || strcmp(e->d_name + l - 6, ".rules") != 0) continue;
            if (l >= sizeof names[0]) continue;
            int found = -1;
            for (int i = 0; i < nn; i++) if (strcmp(names[i], e->d_name) == 0) { found = i; break; }
            if (found < 0) {
                if (nn >= 1024) continue;
                found = nn++;
                safe_copy(names[found], e->d_name, sizeof names[0]);
            }
            snprintf(owner[found], sizeof owner[0], "%s/%s", dirs[di], e->d_name);
        }
        closedir(d);
    }
    /* insertion sort basenames lexically, carrying owner path */
    for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && strcmp(names[j-1], names[j]) > 0; j--) {
            char tn[64]; safe_copy(tn, names[j-1], sizeof tn);
            safe_copy(names[j-1], names[j], sizeof tn); safe_copy(names[j], tn, sizeof tn);
            char to[256]; safe_copy(to, owner[j-1], sizeof to);
            safe_copy(owner[j-1], owner[j], sizeof to); safe_copy(owner[j], to, sizeof to);
        }
    for (int i = 0; i < nn; i++) ruleset_load_file(owner[i], rs);
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall -Wextra -D_GNU_SOURCE tests/test_udev_ruleset.c -o /tmp/t-ruleset && /tmp/t-ruleset`
Expected: `dirs OK`, no assert failures.

- [ ] **Step 5: Commit**

```bash
git add udev_ruleset.h tests/test_udev_ruleset.c
git commit -m "feat(schema-udev): R1 directory loader (udev precedence + lexical order)"
```

---

### Task 5: Wire into `make test` + load the real installed set (smoke)

**Files:**
- Modify: `Makefile:test` target and `.PHONY`/test list as needed.
- Test: `tests/test_udev_ruleset.c`

**Interfaces:**
- Consumes: `ruleset_load_dirs`.
- Produces: no new function — a smoke assertion that the real installed rules load without crashing and yield a plausible rule count.

- [ ] **Step 1: Write the failing test**

Add before the final print (guarded so CI without udev still passes):

```c
    /* smoke: real installed set loads and parses a sane number of rules */
    const char *real[] = { "/usr/lib/udev/rules.d", "/run/udev/rules.d", "/etc/udev/rules.d" };
    struct ruleset live = {0};
    assert(ruleset_load_dirs(real, 3, &live) == 0);
    if (access("/usr/lib/udev/rules.d", F_OK) == 0)
        assert(live.n > 100);   /* 168 files on blakbox -> thousands of rules */
    free(live.rules);

    printf("test_udev_ruleset: ALL OK\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | grep -i ruleset`
Expected: FAIL — the test is not yet in the `test:` target (no `ruleset` line runs).

- [ ] **Step 3: Add the test to the Makefile**

In the `test:` target, after the `test_udev_db` line, add:

```make
	$(CC) $(CFLAGS) tests/test_udev_ruleset.c -o /tmp/schema-test-ruleset && /tmp/schema-test-ruleset
```

- [ ] **Step 4: Run the full suite to verify it passes**

Run: `make test 2>&1 | tail -5`
Expected: `test_udev_ruleset: ALL OK` and no failures across the suite.

- [ ] **Step 5: Commit**

```bash
git add Makefile tests/test_udev_ruleset.c
git commit -m "test(schema-udev): wire R1 ruleset parser into make test + live smoke"
```

---

## Self-Review

**Spec coverage (R1 scope only):**
- Load rules.d in udev precedence, merge by filename, lexical order → Task 4. ✓
- Tokenize lines into `(key, subkey, op, value)` clauses → Tasks 1–2. ✓
- Preserve `LABEL`/`GOTO` as clauses → Task 2 test asserts both. ✓
- Comment/blank/continuation handling → Task 3. ✓
- In-memory `struct ruleset` output → Task 1 struct, Task 3 append. ✓
- R2–R5 (match/execute/import/integrate) are **out of R1 scope** — separate plans. ✓

**Placeholder scan:** No TBD/TODO; every code step has real code. ✓

**Type consistency:** `struct rule_clause`/`struct rule`/`struct ruleset`, `enum rule_op` values, and function signatures (`ruleset_parse_clause`, `ruleset_parse_line`, `ruleset_append`, `ruleset_load_file`, `ruleset_load_dirs`) are defined in Task 1/3/4 and used consistently. ✓

**Known follow-ups (not R1):** value substitution tokens (`$attr{}`, `%k`) are stored verbatim by R1 and expanded in R2; escaped quotes inside values are rare in the installed set and deferred to R2 if a real rule needs them.
