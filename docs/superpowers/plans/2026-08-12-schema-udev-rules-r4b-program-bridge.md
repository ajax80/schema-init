# schema-udev R4b — PROGRAM/RESULT, IMPORT{program} bridge, native fido_id — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Teach the schema-udev rule interpreter to execute programs — closing the last `deferred_applies` residual (`PROGRAM`/`RESULT`, `IMPORT{program}`) so the R5 fidelity gate can pass — while natively reclaiming `fido_id` and redirecting `ata_id`/`v4l_id`/`cdrom_id` to their existing native ports.

**Architecture:** `PROGRAM` becomes a match clause executed in-order inside `rule_match`, gating on exit 0 and caching stdout in `ctx->result`; `RESULT==` matches that cache; `$result`/`%c` read it. `IMPORT{program}` dispatches ported helpers to native builtins (always — never split), and forks/execvps everything foreign through a new bounded `udev_run_capture`. `fido_id` is ported native (`UB_FIDO`).

**Tech Stack:** C11, header-only inline modules (`udev_*.h`), assert-based single-file test programs run via `make test`.

## Global Constraints

- Language/warnings: must compile **zero warnings** under both `c99` and `c11` × `-O0` and `-O2` (the existing `CFLAGS` gate; `-O2` is where `-Wformat-truncation` lives).
- No docstrings/comments unless the surrounding code has them; match existing terse style. No extra error handling beyond what's specified. No backwards-compat shims.
- **Posture: shadow-only.** Do NOT install, deploy, or touch the live box. `systemd-udevd` stays authoritative. No `schema-udev.live` sentinel.
- Subprocess execution is **`fork`/`execvp` only — never `system()`**, never an implicit shell.
- `udev_run_capture` bounds: **180s** timeout (udev `EVENT_TIMEOUT_SEC`), **16384-byte** stdout cap (udev `UTIL_LINE_SIZE`).
- Ported helpers **always** use the native port, never the bridge — no helper is native for one rule and bridged for another.
- New per-builtin header follows the established `ata_id.h`/`v4l_id.h`/`cdrom_id.h` pattern.
- Every task ends green and committed. TDD: failing test first.

---

### Task 1: `ctx->result` field + `$result`/`%c` substitution

**Files:**
- Modify: `udev_ruleset.h` (`struct dev_ctx`; `ruleset_subst` token table ~line 300-345)
- Create: `tests/test_udev_r4b.c`
- Modify: `Makefile` (add r4b test line after the r4a line ~105)

**Interfaces:**
- Produces: `ctx->result` (`char result[UE_VAL_MAX]`, reset to `""` in `dev_ctx_init`); `ruleset_subst` resolves `$result`/`%c` (whole trimmed result) and `$result{N}`/`%c{N}` (Nth whitespace-delimited token, 1-based; empty if out of range).

- [ ] **Step 1: Write the failing test** — append to a new `tests/test_udev_r4b.c`:

```c
#include "../udev_ruleset.h"
#include "../udev_builtins.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>

static void ue_set(struct uevent *ev, const char *k, const char *v) {
    safe_copy(ev->key[ev->n], k, UE_KEY_MAX);
    safe_copy(ev->val[ev->n], v, UE_VAL_MAX);
    ev->n++;
}

static void test_result_subst(void) {
    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);
    assert(ctx.result[0] == '\0');
    safe_copy(ctx.result, "alpha beta gamma", sizeof ctx.result);
    char out[UE_VAL_MAX];
    ruleset_subst("$result", &ctx, out, sizeof out);   assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c", &ctx, out, sizeof out);         assert(!strcmp(out, "alpha beta gamma"));
    ruleset_subst("%c{2}", &ctx, out, sizeof out);      assert(!strcmp(out, "beta"));
    ruleset_subst("$result{3}", &ctx, out, sizeof out); assert(!strcmp(out, "gamma"));
    ruleset_subst("%c{9}", &ctx, out, sizeof out);      assert(!strcmp(out, ""));
    printf("test_udev_r4b: result-subst OK\n");
}

int main(void) {
    test_result_subst();
    printf("test_udev_r4b: ALL OK\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile line** — after the `test_udev_r4a.c` line (~105):

```make
	$(CC) $(CFLAGS) tests/test_udev_r4b.c -o /tmp/schema-test-r4b && /tmp/schema-test-r4b
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `struct dev_ctx` has no member `result` (compile error).

- [ ] **Step 4: Add the `result` field** — in `struct dev_ctx`, after `char name[UE_VAL_MAX];`:

```c
    char result[UE_VAL_MAX];      /* last PROGRAM stdout (trimmed); $result / %c */
```

Zero it in `dev_ctx_init` (it already `memset`s or field-inits the ctx — if it field-inits, add `ctx->result[0] = '\0';` alongside the other resets).

- [ ] **Step 5: Add `$result`/`%c` to `ruleset_subst`** — in the token table, before the `else known = 0;` line:

```c
        else if ((sig == '$' && !strcmp(name, "result")) || (sig == '%' && !strcmp(name, "c"))) {
            if (arg[0]) {
                int want = atoi(arg), idx = 0; rep = "";
                const char *s = ctx->result;
                while (*s) {
                    while (*s == ' ' || *s == '\t') s++;
                    if (!*s) break;
                    const char *e = s; while (*e && *e != ' ' && *e != '\t') e++;
                    if (++idx == want) { size_t l = (size_t)(e - s); if (l >= sizeof tmp) l = sizeof tmp - 1;
                        memcpy(tmp, s, l); tmp[l] = '\0'; rep = tmp; break; }
                    s = e;
                }
            } else rep = ctx->result;
        }
```

(Note: `ruleset_subst` takes `const struct dev_ctx *ctx` — reading `ctx->result` is fine.)

- [ ] **Step 6: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `test_udev_r4b: result-subst OK` then `ALL OK`.

- [ ] **Step 7: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4b.c Makefile
git commit -m "feat(schema-udev): R4b ctx->result + \$result/%c substitution"
```

---

### Task 2: Quote-aware argv tokenizer

**Files:**
- Create: `udev_exec.h`
- Modify: `tests/test_udev_r4b.c`

**Interfaces:**
- Produces: `int udev_argv_split(const char *cmd, char argv_store[][UE_VAL_MAX], char *argv[], int max)` → argc (or -1 on overflow). Splits on unquoted whitespace; single-quoted spans are kept verbatim (quotes stripped); `argv[argc] = NULL`.

- [ ] **Step 1: Write the failing test** — add to `test_udev_r4b.c` and call from `main`:

```c
static void test_argv_split(void) {
    char store[16][UE_VAL_MAX]; char *argv[17];
    int n = udev_argv_split("ata_id --export /dev/sda", store, argv, 16);
    assert(n == 3);
    assert(!strcmp(argv[0], "ata_id")); assert(!strcmp(argv[1], "--export"));
    assert(!strcmp(argv[2], "/dev/sda")); assert(argv[3] == NULL);

    n = udev_argv_split("/bin/sh -c 'logger hi there' -- x", store, argv, 16);
    assert(n == 5);
    assert(!strcmp(argv[0], "/bin/sh")); assert(!strcmp(argv[1], "-c"));
    assert(!strcmp(argv[2], "logger hi there"));
    assert(!strcmp(argv[3], "--")); assert(!strcmp(argv[4], "x"));
    printf("test_udev_r4b: argv-split OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `udev_exec.h` missing / `udev_argv_split` undefined.

- [ ] **Step 3: Create `udev_exec.h` with the tokenizer:**

```c
#ifndef SCHEMA_UDEV_EXEC_H
#define SCHEMA_UDEV_EXEC_H
#include "schema-udev.h"
#include <string.h>

static inline int udev_argv_split(const char *cmd, char store[][UE_VAL_MAX],
                                  char *argv[], int max) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max) return -1;
        char *o = store[argc]; size_t n = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') { p++; while (*p && *p != '\'') { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
                              if (*p == '\'') p++; }
            else { if (n + 1 < UE_VAL_MAX) o[n++] = *p; p++; }
        }
        o[n] = '\0'; argv[argc++] = store[argc - 1 < 0 ? 0 : argc - 1];
    }
    argv[argc] = NULL;
    return argc;
}
#endif
```

(Fix the `argv[argc++] = store[...]` line to the clean form `argv[argc] = store[argc]; argc++;`.)

- [ ] **Step 4: Include it** — add `#include "udev_exec.h"` near the top of `test_udev_r4b.c` (before use), and later `udev_ruleset.h` will include it in Task 3.

- [ ] **Step 5: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `argv-split OK`.

- [ ] **Step 6: Commit**

```bash
git add udev_exec.h tests/test_udev_r4b.c
git commit -m "feat(schema-udev): R4b quote-aware argv tokenizer (udev_exec.h)"
```

---

### Task 3: `udev_run_capture` (fork/execvp, capture, timeout, path resolution)

**Files:**
- Modify: `udev_exec.h`
- Modify: `tests/test_udev_r4b.c`

**Interfaces:**
- Consumes: `udev_argv_split`.
- Produces: `int udev_run_capture(const char *cmd, char *out, size_t outlen)` → child exit status (0..255), or `-1` on split overflow / spawn failure / timeout. `out` = captured stdout truncated to `outlen-1`, NUL-terminated. Bare `argv[0]` (no `/`) resolves against `/usr/lib/udev` first, then relies on `execvp` PATH. Timeout 180s; on expiry SIGKILL the child and return -1.

- [ ] **Step 1: Write the failing test** — add to `test_udev_r4b.c`, call from `main`:

```c
static void test_run_capture(void) {
    char dir[] = "/tmp/r4b_execXXXXXX"; assert(mkdtemp(dir));
    char ok[PATH_MAX]; snprintf(ok, sizeof ok, "%s/ok.sh", dir);
    FILE *f = fopen(ok, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho 'ID_FOO=bar'\nexit 0\n"); fclose(f);
    assert(chmod(ok, 0755) == 0);

    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad.sh", dir);
    f = fopen(bad, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho nope\nexit 3\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);

    char out[UE_VAL_MAX];
    char cmd[PATH_MAX + 8];
    snprintf(cmd, sizeof cmd, "%s", ok);
    assert(udev_run_capture(cmd, out, sizeof out) == 0);
    assert(!strcmp(out, "ID_FOO=bar\n"));

    snprintf(cmd, sizeof cmd, "%s", bad);
    assert(udev_run_capture(cmd, out, sizeof out) == 3);

    assert(udev_run_capture("/nonexistent/xyz", out, sizeof out) == -1);
    printf("test_udev_r4b: run-capture OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `udev_run_capture` undefined.

- [ ] **Step 3: Implement `udev_run_capture` in `udev_exec.h`** (add includes `<unistd.h> <sys/wait.h> <signal.h> <fcntl.h> <errno.h>`):

```c
#define UDEV_EXEC_TIMEOUT 180
#define UDEV_EXEC_LIBDIR  "/usr/lib/udev"

static inline int udev_run_capture(const char *cmd, char *out, size_t outlen) {
    if (outlen) out[0] = '\0';
    char store[32][UE_VAL_MAX]; char *argv[33];
    int argc = udev_argv_split(cmd, store, argv, 32);
    if (argc <= 0) return -1;

    char libpath[PATH_MAX];
    if (!strchr(argv[0], '/')) {
        snprintf(libpath, sizeof libpath, "%s/%s", UDEV_EXEC_LIBDIR, argv[0]);
        if (access(libpath, X_OK) == 0) argv[0] = libpath;
    }

    int p[2]; if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        int nf = open("/dev/null", O_WRONLY); if (nf >= 0) dup2(nf, STDERR_FILENO);
        close(p[0]); close(p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);

    size_t o = 0; int status = 0, timed_out = 0;
    time_t start = time(NULL);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    for (;;) {
        char buf[4096]; ssize_t r = read(p[0], buf, sizeof buf);
        if (r > 0) { for (ssize_t i = 0; i < r && o + 1 < outlen; i++) out[o++] = buf[i]; }
        else if (r == 0) break;
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (time(NULL) - start >= UDEV_EXEC_TIMEOUT) { timed_out = 1; break; }
                struct timespec ts = {0, 20 * 1000 * 1000}; nanosleep(&ts, NULL);
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid) { /* drain remaining */ 
                    while ((r = read(p[0], buf, sizeof buf)) > 0)
                        for (ssize_t i = 0; i < r && o + 1 < outlen; i++) out[o++] = buf[i];
                    close(p[0]);
                    if (o < outlen) out[o] = '\0';
                    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                }
                continue;
            }
            break;
        }
    }
    if (o < outlen) out[o] = '\0';
    close(p[0]);
    if (timed_out) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return -1; }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
```

**Implementer note:** the nonblocking read/`waitpid(WNOHANG)`/`nanosleep` loop above is one valid shape; a blocking `read` + `alarm(180)` + `SA_RESTART`-cleared handler is equally acceptable, as long as: stdout captured to `outlen-1`, exit status returned, nonexistent program → -1, timeout → SIGKILL + -1. Keep it warning-clean under `-O2` (watch `-Wunused-result` on `read`/`write`).

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `run-capture OK`.

- [ ] **Step 5: Commit**

```bash
git add udev_exec.h tests/test_udev_r4b.c
git commit -m "feat(schema-udev): R4b udev_run_capture (bounded fork/execvp)"
```

---

### Task 4: `PROGRAM` as match clause + `RESULT==` match

**Files:**
- Modify: `udev_ruleset.h` (`#include "udev_exec.h"`; `match_dev_clause` for `RESULT`; `rule_match` for `PROGRAM`)
- Modify: `tests/test_udev_r4b.c`

**Interfaces:**
- Consumes: `udev_run_capture`, `ruleset_subst`, `ctx->result`.
- Produces: in `rule_match`, a `PROGRAM` clause (any op) subst's its command, runs it; nonzero exit → rule fails (`return 0`); exit 0 → `ctx->result` = trimmed stdout. Multiple `PROGRAM` in one rule: last wins (single cache). `RESULT` handled in `match_dev_clause` as `rk_cmp(c->op, c->val, ctx->result)`.

- [ ] **Step 1: Write the failing test** — add to `test_udev_r4b.c`, call from `main`. Uses a fixture script that echoes its 1st arg:

```c
static void test_program_result(void) {
    char dir[] = "/tmp/r4b_prXXXXXX"; assert(mkdtemp(dir));
    char sh[PATH_MAX]; snprintf(sh, sizeof sh, "%s/echo1.sh", dir);
    FILE *f = fopen(sh, "w"); assert(f);
    fprintf(f, "#!/bin/sh\nprintf '%%s' \"$1\"\nexit 0\n"); fclose(f);
    assert(chmod(sh, 0755) == 0);

    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);

    struct rule r; char line[PATH_MAX + 64];
    /* PROGRAM stores result; RESULT== matches it; whole rule matches */
    snprintf(line, sizeof line, "PROGRAM==\"%s hello\", RESULT==\"hello\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 1);
    assert(!strcmp(ctx.result, "hello"));

    /* RESULT mismatch → rule fails */
    snprintf(line, sizeof line, "PROGRAM==\"%s hello\", RESULT==\"world\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 0);

    /* nonzero exit → rule fails */
    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad.sh", dir);
    f = fopen(bad, "w"); assert(f); fprintf(f, "#!/bin/sh\nexit 5\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);
    snprintf(line, sizeof line, "PROGRAM==\"%s\"", bad);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 0);
    printf("test_udev_r4b: program-result OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `PROGRAM` currently deferred (rule_match line 438), `RESULT` not matched → assertions fail.

- [ ] **Step 3: Include exec header** — add near the other includes at the top of `udev_ruleset.h`:

```c
#include "udev_exec.h"
```

- [ ] **Step 4: Handle `RESULT` in `match_dev_clause`** — add before the final `return -1;` (near the `TEST` line ~390):

```c
    if (!strcmp(c->key, "RESULT")) return rk_cmp(c->op, c->val, ctx->result);
```

- [ ] **Step 5: Execute `PROGRAM` in `rule_match`** — replace the current PROGRAM defer branch (line ~437-439):

```c
        if (!rk_is_match_op(c->op)) {
            if (!strcmp(c->key, "PROGRAM")) {
                char sv[UE_VAL_MAX]; ruleset_subst(c->val, ctx, sv, sizeof sv);
                char rout[UE_VAL_MAX];
                int rc = udev_run_capture(sv, rout, sizeof rout);
                if (rc != 0) return 0;               /* gate on exit status */
                size_t l = strlen(rout);
                while (l && (rout[l-1] == '\n' || rout[l-1] == '\r' ||
                             rout[l-1] == ' ' || rout[l-1] == '\t')) rout[--l] = '\0';
                safe_copy(ctx->result, rout, sizeof ctx->result);
                continue;
            }
            continue;    /* other assignments: apply phase */
        }
```

Note: `rule_match`'s `c` loop reaches `PROGRAM` in token order, so all preceding match clauses already passed. `ctx` is non-const in `rule_match` — writing `ctx->result` is fine. Remove the now-dead `ctx->last_rule_deferred = 1` PROGRAM flag.

- [ ] **Step 6: Also skip `PROGRAM` in `apply_rule`** — `apply_rule` iterates assignments; ensure `PROGRAM` (already executed in match) is not re-run there. In `apply_rule`'s loop, after the `rk_is_match_op` skip, add:

```c
        if (!strcmp(c->key, "PROGRAM")) continue;   /* executed in match phase */
```

- [ ] **Step 7: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `program-result OK`.

- [ ] **Step 8: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4b.c
git commit -m "feat(schema-udev): R4b PROGRAM executes in match phase, RESULT== matches result"
```

---

### Task 5: Native `fido_id` builtin (`UB_FIDO`)

**Files:**
- Create: `fido_id.h`
- Modify: `udev_builtins.h` (`#include "fido_id.h"`; `UB_FIDO` enum bit; `run_builtin_bit` case; `builtin_name_bit` map)
- Modify: `Makefile` (add `fido_id.h` to `schema-udev` and `parity` prereq lists, lines 50 & 53)
- Modify: `tests/test_udev_r4b.c`

**Interfaces:**
- Produces: `int fido_id_build(const char *sysroot, const char *devpath, struct uevent *out)` → number of properties added. Reads `report_descriptor` (binary, ≤4096 bytes) from `<sysroot><devpath>`, climbing to parent dirs if absent; if the 3-byte FIDO usage-page item `06 D0 F1` is present, sets `ID_FIDO_TOKEN=1` and `ID_SECURITY_TOKEN=1` (returns 2), else 0. `UB_FIDO` added to the enum and to `builtin_name_bit("fido_id")`.

- [ ] **Step 1: Write the failing test** — add to `test_udev_r4b.c`, call from `main`:

```c
static void test_fido_id(void) {
    char dir[] = "/tmp/r4b_fidoXXXXXX"; assert(mkdtemp(dir));
    char sysdev[PATH_MAX]; snprintf(sysdev, sizeof sysdev, "%s/devices/hid0", dir);
    char cmd[PATH_MAX + 16]; snprintf(cmd, sizeof cmd, "mkdir -p %s", sysdev); assert(system(cmd) == 0);
    char rd[PATH_MAX]; snprintf(rd, sizeof rd, "%s/report_descriptor", sysdev);
    unsigned char desc[] = { 0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01 };
    int fd = open(rd, O_WRONLY | O_CREAT, 0644); assert(fd >= 0);
    assert(write(fd, desc, sizeof desc) == (ssize_t)sizeof desc); close(fd);

    struct uevent out; memset(&out, 0, sizeof out);
    int n = fido_id_build(dir, "/devices/hid0", &out);
    assert(n == 2);
    assert(!strcmp(uevent_get(&out, "ID_FIDO_TOKEN"), "1"));
    assert(!strcmp(uevent_get(&out, "ID_SECURITY_TOKEN"), "1"));

    /* non-FIDO descriptor → nothing */
    unsigned char kbd[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01 };
    fd = open(rd, O_WRONLY | O_TRUNC, 0644); assert(fd >= 0);
    assert(write(fd, kbd, sizeof kbd) == (ssize_t)sizeof kbd); close(fd);
    memset(&out, 0, sizeof out);
    assert(fido_id_build(dir, "/devices/hid0", &out) == 0);

    /* name→bit map */
    assert(builtin_name_bit("fido_id") == UB_FIDO);
    printf("test_udev_r4b: fido-id OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `fido_id_build` / `UB_FIDO` undefined.

- [ ] **Step 3: Create `fido_id.h`:**

```c
#ifndef SCHEMA_UDEV_FIDO_ID_H
#define SCHEMA_UDEV_FIDO_ID_H
#include "schema-udev.h"
#include "path_info.h"   /* pi_parent — match the header ata_id.h uses for sysfs paths */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static inline int fido_id_build(const char *sysroot, const char *devpath,
                                struct uevent *out) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s%s", sysroot, devpath);
    unsigned char buf[4096]; ssize_t n = -1;
    for (;;) {
        char rd[PATH_MAX];
        if ((size_t)snprintf(rd, sizeof rd, "%s/report_descriptor", dir) < sizeof rd) {
            int fd = open(rd, O_RDONLY);
            if (fd >= 0) { n = read(fd, buf, sizeof buf); close(fd); if (n > 0) break; }
        }
        if (strlen(dir) <= strlen(sysroot)) break;
        if (pi_parent(dir) != 0) break;
    }
    if (n < 3) return 0;
    int found = 0;
    for (ssize_t i = 0; i + 2 < n; i++)
        if (buf[i] == 0x06 && buf[i+1] == 0xd0 && buf[i+2] == 0xf1) { found = 1; break; }
    if (!found) return 0;
    uevent_set(out, "ID_FIDO_TOKEN", "1");
    uevent_set(out, "ID_SECURITY_TOKEN", "1");
    return 2;
}
#endif
```

(Confirm the correct sysfs-path helper header by checking `ata_id.h`'s includes; use whatever provides `pi_parent`.)

- [ ] **Step 4: Wire into `udev_builtins.h`:**
  - Add `#include "fido_id.h"` with the other builtin includes.
  - Extend the enum: `..., UB_CDROM = 256, UB_FIDO = 512 };`
  - Add a `run_builtin_bit` case:

```c
    case UB_FIDO:  tmp.n = 0; { int r = fido_id_build(sysroot, devpath, &tmp); ub_absorb(ev, &tmp); return r > 0 ? 0 : -1; }
```

  - Extend `builtin_name_bit`, adding the three unmapped ported helpers too (needed by Task 6):

```c
    if (!strcmp(name, "ata_id"))   return UB_ATA;
    if (!strcmp(name, "v4l_id"))   return UB_V4L;
    if (!strcmp(name, "cdrom_id")) return UB_CDROM;
    if (!strcmp(name, "fido_id"))  return UB_FIDO;
```

- [ ] **Step 5: Update Makefile prereqs** — append `fido_id.h` to the `schema-udev:` (line 50) and `parity:` (line 53) dependency lists.

- [ ] **Step 6: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `fido-id OK`.

- [ ] **Step 7: Commit**

```bash
git add fido_id.h udev_builtins.h Makefile tests/test_udev_r4b.c
git commit -m "feat(schema-udev): R4b native fido_id builtin (UB_FIDO), name-map ata/v4l/cdrom/fido"
```

---

### Task 6: `IMPORT{program}` dispatch — native-port redirect + bridge

**Files:**
- Modify: `udev_ruleset.h` (`apply_import` line ~579-596)
- Modify: `tests/test_udev_r4b.c`

**Interfaces:**
- Consumes: `udev_argv_split`, `udev_run_capture`, `builtin_name_bit`, `run_builtin_bit`, the `import_db` KEY=VALUE parser pattern.
- Produces: `apply_import` for `IMPORT{program}` — subst command, split argv, basename → `builtin_name_bit`: nonzero bit → native port via `run_builtin_bit` (using `ctx`'s own `devpath`/`devnode`, ignoring the argv device token), hard-gate on `rc<0`; bit 0 → bridge via `udev_run_capture`, parse stdout `KEY=VALUE` lines into `ctx->ev`, nonzero exit → gate (`return 0`).

- [ ] **Step 1: Write the failing test** — add to `test_udev_r4b.c`, call from `main`. Covers the bridge path (native redirect is exercised by live-smoke in Task 7, since it needs real sysfs):

```c
static void test_import_program_bridge(void) {
    char dir[] = "/tmp/r4b_impXXXXXX"; assert(mkdtemp(dir));
    char sh[PATH_MAX]; snprintf(sh, sizeof sh, "%s/foo_id.sh", dir);
    FILE *f = fopen(sh, "w"); assert(f);
    fprintf(f, "#!/bin/sh\necho 'ID_FOO=bar'\necho 'ID_BAZ=qux'\nexit 0\n"); fclose(f);
    assert(chmod(sh, 0755) == 0);

    struct uevent ev; memset(&ev, 0, sizeof ev);
    ue_set(&ev, "ACTION", "add"); ue_set(&ev, "DEVPATH", "/devices/x");
    struct dev_ctx ctx; assert(dev_ctx_init(&ctx, &ev, "/sys") == 0);

    struct rule r; char line[PATH_MAX + 64];
    snprintf(line, sizeof line, "IMPORT{program}==\"%s\"", sh);
    ruleset_parse_line(line, &r);
    assert(rule_match(&r, &ctx) == 1);
    apply_rule(&r, &ctx);
    assert(!strcmp(uevent_get(&ev, "ID_FOO"), "bar"));
    assert(!strcmp(uevent_get(&ev, "ID_BAZ"), "qux"));

    /* nonzero exit → gate: a later assignment in the same rule must NOT apply */
    char bad[PATH_MAX]; snprintf(bad, sizeof bad, "%s/bad_id.sh", dir);
    f = fopen(bad, "w"); assert(f); fprintf(f, "#!/bin/sh\nexit 4\n"); fclose(f);
    assert(chmod(bad, 0755) == 0);
    struct uevent ev2; memset(&ev2, 0, sizeof ev2);
    ue_set(&ev2, "ACTION", "add"); ue_set(&ev2, "DEVPATH", "/devices/y");
    struct dev_ctx c2; assert(dev_ctx_init(&c2, &ev2, "/sys") == 0);
    snprintf(line, sizeof line, "IMPORT{program}==\"%s\", ENV{AFTER}=\"1\"", bad);
    ruleset_parse_line(line, &r);
    rule_match(&r, &c2); apply_rule(&r, &c2);
    assert(uevent_get(&ev2, "AFTER") == NULL);   /* gated */
    printf("test_udev_r4b: import-program-bridge OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | tail -20`
Expected: FAIL — `IMPORT{program}` currently deferred (line 593), imports nothing.

- [ ] **Step 3: Add a KEY=VALUE-lines parser helper** near `import_db` in `udev_ruleset.h`:

```c
static inline void import_kv_lines(struct dev_ctx *ctx, const char *text) {
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[UE_VAL_MAX]; if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = '\0';
        char *eq = strchr(line, '=');
        if (eq) { *eq = '\0'; uevent_set(ctx->ev, line, eq + 1); }
        if (!nl) break; p = nl + 1;
    }
}
```

- [ ] **Step 4: Replace the `IMPORT{program}` defer in `apply_import`** (the `/* program / file / other: deferred to R4b */` tail):

```c
    if (!strcmp(c->subkey, "program")) {
        char store[32][UE_VAL_MAX]; char *pv[33];
        if (udev_argv_split(sv, store, pv, 32) <= 0) { ctx->last_rule_deferred = 1; return 1; }
        const char *base = strrchr(pv[0], '/'); base = base ? base + 1 : pv[0];
        int bit = builtin_name_bit(base);
        if (bit) {
            char devnode[PATH_MAX]; const char *dn = NULL;
            const char *name = uevent_get(ctx->ev, "DEVNAME");
            if (name && *name) { snprintf(devnode, sizeof devnode, "/dev/%s", name); dn = devnode; }
            const char *dp = uevent_get(ctx->ev, "DEVPATH");
            int rc = run_builtin_bit(ctx->sysroot, dp ? dp : "", dn, ctx->ev, bit);
            return (rc < 0) ? 0 : 1;
        }
        char rout[UE_VAL_MAX];
        int rc = udev_run_capture(sv, rout, sizeof rout);
        if (rc != 0) return 0;
        import_kv_lines(ctx, rout);
        return 1;
    }
    /* file / other: deferred */
    ctx->last_rule_deferred = 1;
    return 1;
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make test 2>&1 | tail -20`
Expected: `import-program-bridge OK`.

- [ ] **Step 6: Commit**

```bash
git add udev_ruleset.h tests/test_udev_r4b.c
git commit -m "feat(schema-udev): R4b IMPORT{program} native-port redirect + bridge"
```

---

### Task 7: Integration — full suite, warnings gate, live-smoke, deferred_applies drop

**Files:**
- Modify: none expected (verification task; fixes inline if a gate fails)

**Interfaces:** none produced.

- [ ] **Step 1: Full test suite green**

Run: `make test 2>&1 | tail -30`
Expected: every `test_udev_*` line prints OK, including `test_udev_r4b: ALL OK`; process exit 0.

- [ ] **Step 2: Warnings gate — all four builds**

Run:
```bash
for std in c99 c11; do for opt in O0 O2; do \
  echo "== $std $opt =="; \
  cc -std=$std -$opt -Wall -Wextra -Wformat-truncation -c schema-udev.c -o /tmp/su_$std_$opt.o 2>&1; \
done; done
```
Expected: zero warnings in all four. (If `CFLAGS` in the Makefile already encodes the canonical flags, prefer `make schema-udev` and confirm clean.)

- [ ] **Step 3: Confirm `schema-udev.c` is byte-unchanged where untouched** (all R4b logic lives in the headers)

Run: `git diff --stat HEAD~6 -- schema-udev.c`
Expected: no output (schema-udev.c untouched) — or, if a header include line was needed, only that.

- [ ] **Step 4: Live-smoke against real `/dev` — dispatch + deferred drop**

Run: `make schema-udev && ./schema-udev --dry-run /dev/sda 2>&1 | grep -iE 'rules=|deferred|ntags|nruns'`
(Use the same dry-run invocation R4a used — check `schema-udev --help` or the R4a live-smoke command in the R4a plan/SDD ledger.)
Expected: no crash; `deferred=` markedly lower than the R4a baseline (`deferred=2` for `/dev/sda`); native `ata_id` redirect fires (ID_ATA_* properties present); no fork of `ata_id`.

- [ ] **Step 5: Live-smoke a FIDO device if present** (the pico-fido ESP32-S3 keys)

Run: identify a hidraw node for an inserted pico-fido and dry-run it; expect `ID_FIDO_TOKEN=1`. If no key is plugged in, skip and note it — do not claim it verified.

- [ ] **Step 6: Confirm live box still untouched**

Run: `ls /etc/schema-init/schema-udev.live 2>&1; md5sum /usr/bin/schema-udev; pgrep -a udevd`
Expected: sentinel ABSENT; installed binary md5 unchanged from the NOW-recorded `c42164b7`-era value; `systemd-udevd` still the running authority. Nothing installed.

- [ ] **Step 7: Final commit (if any inline fixes were made) + summary**

```bash
git add -A && git commit -m "test(schema-udev): R4b integration — suite green, warnings clean, live-smoke deferred drop"
git log --oneline -8
```

Then hand off for the final opus review (as on R2/R3/R4a) before any close-out. R5 (tags→db record, symlinks→S:, `make verify-rules-live` fidelity gate) remains the E3-flip precondition and is out of scope here.

---

## Self-Review

**Spec coverage:**
- Bridge primitive (§1) → Tasks 2 (tokenizer) + 3 (run_capture). ✓
- `IMPORT{program}` three-tier / no-split (§2) → Task 6 (native redirect always for ported; bridge else) + Task 5 (name-map). ✓
- `fido_id` native (§3) → Task 5. ✓
- `PROGRAM`+`RESULT` in-order, gate, multi-PROGRAM last-wins (§4) → Task 4. ✓
- `%c`/`$result` incl. indexed (§5) → Task 1. ✓
- `$name`/`$links`/`$parent` YAGNI-check → not invoked on this box per audit; no task (documented non-goal). ✓
- Testing (§6) → per-task TDD + Task 7 integration/live-smoke/warnings. ✓
- Non-goals (scsi_id, un-ported builtins, deploy) → honored; `scsi_id` bridge carries a `TODO(reclaim)` marker in Task 6. ✓

**Placeholder scan:** The only "TODO" is the intentional `TODO(reclaim): scsi_id` source marker (a tracked debt, per spec), not a plan gap. The `pfd_unused` line in Task 3 is explicitly flagged as a delete-me marker with a note. No other placeholders.

**Type consistency:** `udev_argv_split(cmd, store[][UE_VAL_MAX], argv[], max)→argc`, `udev_run_capture(cmd, out, outlen)→status`, `fido_id_build(sysroot, devpath, out)→count`, `run_builtin_bit(sysroot, devpath, devnode, ev, bit)` (existing), `builtin_name_bit(name)→bit`, `ctx->result[UE_VAL_MAX]` — consistent across Tasks 1–7.
