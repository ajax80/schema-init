# schema-systemctl shim Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `schema-systemctl`, a `systemctl(1)` drop-in that makes RPM package scriptlets succeed on a schema-init box and captures enablement intent to a pending queue for the later importer.

**Architecture:** All logic lives in a static-inline header `systemctl_shim.h` (matching the repo's udev/sdbus header-carried-logic pattern); `schema-systemctl.c` is a thin `main()` that calls `shim_dispatch(argc, argv)`. Path locations and the `schema-ctl` program name are env-overridable so tests run without root, a live PID 1, or the real filesystem. RPM `-migrate` scriptlets divert `/usr/bin/systemctl` to the shim and self-heal across systemd upgrades.

**Tech Stack:** C (c99, libc only, `-Wall -Wextra`), RPM spec scriptlets, C assert-based tests compiled per the existing `make test` recipe, one Python spec test.

**Spec:** `docs/superpowers/specs/2026-09-14-schema-systemctl-shim-design.md`

## Global Constraints

- **Language:** C99, libc only. `CFLAGS += -std=c99 -Wall -Wextra -D_GNU_SOURCE -I.` (Makefile appends these; don't fight them). No new runtime deps.
- **House style:** no comments/docstrings beyond what clarifies non-obvious intent; no extra error handling beyond what's required; edit existing files, create new ones only where the plan says Create.
- **Binary ships in the `schema-init-migrate` subpackage only**, never the base package.
- **Exit-code contract (load-bearing):** every verb exits `0` EXCEPT `is-enabled` (`1` when not enabled) and `is-active` (`3` when inactive). Nothing else may abort a scriptlet.
- **Paths (all env-overridable for tests):**
  - state dir: `$SCHEMA_STATE_DIR` else `/var/lib/schema-init`; queue file = `<state_dir>/pending.list`
  - svc dir: `$SCHEMA_SVC_DIR` else `/etc/schema-init/services`; svc file = `<svc_dir>/<name>.svc`
  - unit search dir: `$SCHEMA_UNIT_DIR` else the ordered list `/etc/systemd/system`, `/usr/lib/systemd/system`, `/lib/systemd/system`
  - control program: `$SCHEMA_CTL` else `schema-ctl`
- **`schema-ctl status --kv` format:** lines `service.<name>.state=<STATE>`. "Active" = the name is present AND its state is NOT one of `DORMANT`, `EXCISED`, `UNKNOWN`.
- **Unsupported unit types (log-and-skip, exit 0):** any unit containing `@`, or ending `.socket`/`.timer`/`.path`/`.target`/`.mount`/`.slice`/`.scope`.

---

## File Structure

- **Create** `systemctl_shim.h` — all shim logic as `static` functions (path knobs, unit normalization, unit-path resolution, queue ops, svc/ctl helpers, `shim_dispatch`).
- **Create** `schema-systemctl.c` — thin `main()`.
- **Create** `tests/test_systemctl_shim.c` — assert-based unit tests over the header, using a `mkdtemp` sandbox and a recording `SCHEMA_CTL` stub.
- **Create** `tests/verify_systemctl_shim.sh` — live exit-code-contract check against the built binary (documentation/live parity, not part of `make test`).
- **Create** `tests/test_spec_systemctl_shim.py` — asserts the spec's scriptlet + `%files` wiring.
- **Modify** `Makefile` — build/install/clean wiring for `schema-systemctl`; add the C test line to `test:`.
- **Modify** `schema-init.spec` — add to `migrate_bins`, `%files migrate`, and the `%post`/`%postun`/`%transfiletriggerin migrate` scriptlets.

---

## Task 1: Header skeleton, build wiring, unit normalization

**Files:**
- Create: `systemctl_shim.h`
- Create: `schema-systemctl.c`
- Create: `tests/test_systemctl_shim.c`
- Modify: `Makefile` (bin rule + `all`/`clean`/`install-migrate`; add test line to `test:`)

**Interfaces:**
- Produces:
  - `static const char *shim_state_dir(void);`
  - `static const char *shim_svc_dir(void);`
  - `static const char *shim_ctl(void);`
  - `static const char *strip_service_suffix(const char *unit, char *buf, size_t n);` — writes the name with a trailing `.service` removed into `buf`, returns `buf`.
  - `static int unit_supported(const char *unit);` — `0` if it contains `@` or ends in an unsupported type suffix, else `1`.
  - `static int shim_dispatch(int argc, char **argv);` — stub returning `0` for now.

- [ ] **Step 1: Write the failing test**

Create `tests/test_systemctl_shim.c`:

```c
#include "../systemctl_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_strip_suffix(void) {
    char b[256];
    assert(strcmp(strip_service_suffix("foo.service", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo", b, sizeof b), "foo") == 0);
    assert(strcmp(strip_service_suffix("foo.socket", b, sizeof b), "foo.socket") == 0);
}

static void test_supported(void) {
    assert(unit_supported("foo.service") == 1);
    assert(unit_supported("foo") == 1);
    assert(unit_supported("foo@bar.service") == 0);
    assert(unit_supported("getty@tty1.service") == 0);
    assert(unit_supported("foo.socket") == 0);
    assert(unit_supported("foo.timer") == 0);
    assert(unit_supported("foo.path") == 0);
    assert(unit_supported("foo.target") == 0);
    assert(unit_supported("foo.mount") == 0);
    assert(unit_supported("foo.slice") == 0);
    assert(unit_supported("foo.scope") == 0);
}

int main(void) {
    test_strip_suffix();
    test_supported();
    printf("task1 systemctl-shim tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: FAIL — `systemctl_shim.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `systemctl_shim.h`:

```c
#ifndef SYSTEMCTL_SHIM_H
#define SYSTEMCTL_SHIM_H

#include <stdlib.h>
#include <string.h>

static const char *shim_state_dir(void) {
    const char *e = getenv("SCHEMA_STATE_DIR");
    return (e && *e) ? e : "/var/lib/schema-init";
}
static const char *shim_svc_dir(void) {
    const char *e = getenv("SCHEMA_SVC_DIR");
    return (e && *e) ? e : "/etc/schema-init/services";
}
static const char *shim_ctl(void) {
    const char *e = getenv("SCHEMA_CTL");
    return (e && *e) ? e : "schema-ctl";
}

static const char *strip_service_suffix(const char *unit, char *buf, size_t n) {
    size_t len = strlen(unit);
    const char *suf = ".service";
    size_t slen = strlen(suf);
    if (len > slen && strcmp(unit + len - slen, suf) == 0)
        len -= slen;
    if (len >= n) len = n - 1;
    memcpy(buf, unit, len);
    buf[len] = '\0';
    return buf;
}

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lu = strlen(suf);
    return ls >= lu && strcmp(s + ls - lu, suf) == 0;
}

static int unit_supported(const char *unit) {
    static const char *bad[] = {
        ".socket", ".timer", ".path", ".target",
        ".mount", ".slice", ".scope", NULL
    };
    int i;
    if (strchr(unit, '@')) return 0;
    for (i = 0; bad[i]; i++)
        if (ends_with(unit, bad[i])) return 0;
    return 1;
}

static int shim_dispatch(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}

#endif
```

Create `schema-systemctl.c`:

```c
#include "systemctl_shim.h"

int main(int argc, char **argv) {
    return shim_dispatch(argc, argv);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: PASS — `task1 systemctl-shim tests passed`.

- [ ] **Step 5: Wire the Makefile**

In `Makefile`: add `schema-systemctl` to the default `BINS ?=` list. Add a build rule next to the `schema-ctl:` rule:

```make
schema-systemctl: schema-systemctl.c systemctl_shim.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
```

Add `schema-systemctl` to the `clean:` `rm -f` list. In the `install-migrate:` recipe, install the binary to `$(DESTDIR)$(PREFIX)/bin/schema-systemctl` (mirror how `schema-udev` is installed there — same `install -m 0755` idiom). Add this line to the `test:` recipe (after the last C test line):

```make
	$(CC) $(CFLAGS) tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl
```

- [ ] **Step 6: Verify build + test via make**

Run: `make schema-systemctl && make test 2>&1 | tail -5`
Expected: binary builds clean (no warnings), `test:` recipe reaches and passes the new line.

- [ ] **Step 7: Commit**

```bash
git add systemctl_shim.h schema-systemctl.c tests/test_systemctl_shim.c Makefile
git commit -m "feat(systemctl-shim): header skeleton, build wiring, unit normalization"
```

---

## Task 2: Unit-path resolution, pending queue, enable/preset/disable/is-enabled

**Files:**
- Modify: `systemctl_shim.h`
- Modify: `tests/test_systemctl_shim.c`

**Interfaces:**
- Consumes: `strip_service_suffix`, `unit_supported`, `shim_state_dir`, `shim_svc_dir` (Task 1).
- Produces:
  - `static char *resolve_unit_path(const char *unit, char *buf, size_t n);` — first existing unit file across the search dirs, else the bare `unit` copied into `buf`. Returns `buf`.
  - `static int queue_path(char *buf, size_t n);` — writes `<state_dir>/pending.list` into `buf`, returns `0`.
  - `static int queue_add(const char *line);` — append if absent (dedup); `mkdir -p` state dir; returns `0`.
  - `static int queue_remove(const char *line);` — rewrite file without `line`; `0` even if absent.
  - `static int queue_contains(const char *line);` — `1`/`0`.
  - `static int svc_exists(const char *name);` — `<svc_dir>/<name>.svc` exists.
  - `shim_dispatch` now handles `enable`, `preset`, `disable`, `is-enabled` (units only, no flags yet).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_systemctl_shim.c` (add calls in `main` too). This uses a sandbox dir + `setenv`:

```c
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static char sandbox[256];

static void setup_sandbox(void) {
    char tmpl[] = "/tmp/shimtestXXXXXX";
    char *d = mkdtemp(tmpl);
    assert(d);
    snprintf(sandbox, sizeof sandbox, "%s", d);
    char state[300], svc[300], unitdir[300];
    snprintf(state, sizeof state, "%s/state", sandbox);
    snprintf(svc, sizeof svc, "%s/svc", sandbox);
    snprintf(unitdir, sizeof unitdir, "%s/units", sandbox);
    mkdir(state, 0755); mkdir(svc, 0755); mkdir(unitdir, 0755);
    setenv("SCHEMA_STATE_DIR", state, 1);
    setenv("SCHEMA_SVC_DIR", svc, 1);
    setenv("SCHEMA_UNIT_DIR", unitdir, 1);
}

static int run(const char *a, const char *b) {
    char *argv[4]; int n = 0;
    argv[n++] = (char *)"systemctl";
    argv[n++] = (char *)a;
    if (b) argv[n++] = (char *)b;
    argv[n] = NULL;
    return shim_dispatch(n, argv);
}

static void write_unit(const char *name) {
    char p[400], *d = getenv("SCHEMA_UNIT_DIR");
    snprintf(p, sizeof p, "%s/%s", d, name);
    FILE *f = fopen(p, "w"); assert(f); fputs("[Service]\n", f); fclose(f);
}

static void test_enable_queue(void) {
    setup_sandbox();
    write_unit("foo.service");
    assert(run("enable", "foo.service") == 0);
    char qp[400];
    snprintf(qp, sizeof qp, "%s/state/pending.list", sandbox);
    FILE *f = fopen(qp, "r"); assert(f);
    char line[400]; int count = 0;
    while (fgets(line, sizeof line, f)) count++;
    fclose(f);
    assert(count == 1);
    /* dedup */
    assert(run("enable", "foo.service") == 0);
    f = fopen(qp, "r"); count = 0;
    while (fgets(line, sizeof line, f)) count++;
    fclose(f);
    assert(count == 1);
    /* is-enabled true */
    assert(run("is-enabled", "foo") == 0);
    /* disable removes */
    assert(run("disable", "foo") == 0);
    assert(run("is-enabled", "foo") == 1);
    /* skip unsupported, still exit 0, not queued */
    assert(run("enable", "foo@bar.service") == 0);
    assert(run("is-enabled", "foo@bar.service") == 1);
    /* preset behaves like enable */
    assert(run("preset", "foo.service") == 0);
    assert(run("is-enabled", "foo") == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: FAIL — dispatch returns 0 but writes no queue file; the `fopen(qp)` assert fails.

- [ ] **Step 3: Write minimal implementation**

Add to `systemctl_shim.h` (above `shim_dispatch`; add `#include <stdio.h>`, `#include <sys/stat.h>`, `#include <unistd.h>` at top):

```c
static char *resolve_unit_path(const char *unit, char *buf, size_t n) {
    const char *one = getenv("SCHEMA_UNIT_DIR");
    const char *dirs[4]; int i, nd = 0;
    if (one && *one) {
        dirs[nd++] = one;
    } else {
        dirs[nd++] = "/etc/systemd/system";
        dirs[nd++] = "/usr/lib/systemd/system";
        dirs[nd++] = "/lib/systemd/system";
    }
    for (i = 0; i < nd; i++) {
        snprintf(buf, n, "%s/%s", dirs[i], unit);
        if (access(buf, F_OK) == 0) return buf;
    }
    snprintf(buf, n, "%s", unit);
    return buf;
}

static int queue_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/pending.list", shim_state_dir());
    return 0;
}

static int queue_contains(const char *line) {
    char qp[512], cur[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(cur, sizeof cur, f)) {
        cur[strcspn(cur, "\n")] = '\0';
        if (strcmp(cur, line) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static int queue_add(const char *line) {
    if (queue_contains(line)) return 0;
    mkdir(shim_state_dir(), 0755);
    char qp[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", line);
    fclose(f);
    return 0;
}

static int queue_remove(const char *line) {
    char qp[512], tmp[512], cur[512];
    queue_path(qp, sizeof qp);
    FILE *f = fopen(qp, "r");
    if (!f) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", qp);
    FILE *o = fopen(tmp, "w");
    if (!o) { fclose(f); return -1; }
    while (fgets(cur, sizeof cur, f)) {
        char trimmed[512];
        snprintf(trimmed, sizeof trimmed, "%s", cur);
        trimmed[strcspn(trimmed, "\n")] = '\0';
        if (strcmp(trimmed, line) != 0) fputs(cur, o);
    }
    fclose(f); fclose(o);
    rename(tmp, qp);
    return 0;
}

static int svc_exists(const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s.svc", shim_svc_dir(), name);
    return access(p, F_OK) == 0;
}
```

Replace the stub `shim_dispatch` with:

```c
static int shim_dispatch(int argc, char **argv) {
    if (argc < 2) return 0;
    const char *verb = argv[1];
    int i;

    if (strcmp(verb, "enable") == 0 || strcmp(verb, "preset") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) continue;
            char path[512];
            resolve_unit_path(argv[i], path, sizeof path);
            queue_add(path);
        }
        return 0;
    }
    if (strcmp(verb, "disable") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) continue;
            char path[512], name[256];
            resolve_unit_path(argv[i], path, sizeof path);
            queue_remove(path);
            strip_service_suffix(argv[i], name, sizeof name);
            char svc[512];
            snprintf(svc, sizeof svc, "%s/%s.svc", shim_svc_dir(), name);
            unlink(svc);
        }
        return 0;
    }
    if (strcmp(verb, "is-enabled") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) return 1;
            char path[512], name[256];
            resolve_unit_path(argv[i], path, sizeof path);
            strip_service_suffix(argv[i], name, sizeof name);
            if (queue_contains(path) || svc_exists(name)) return 0;
            return 1;
        }
        return 1;
    }
    return 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add systemctl_shim.h tests/test_systemctl_shim.c
git commit -m "feat(systemctl-shim): pending queue + enable/preset/disable/is-enabled"
```

---

## Task 3: Lifecycle passthrough + is-active + daemon-reload (schema-ctl coupling)

**Files:**
- Modify: `systemctl_shim.h`
- Modify: `tests/test_systemctl_shim.c`

**Interfaces:**
- Consumes: `svc_exists`, `strip_service_suffix`, `shim_ctl` (Tasks 1–2).
- Produces:
  - `static int run_ctl(const char *verb, const char *name);` — fork/exec `<shim_ctl()> verb name`, wait, return child exit status (or `-1` if spawn failed / socket absent).
  - `static int ctl_is_active(const char *name);` — run `<shim_ctl()> status --kv`, capture stdout, return `1` if a line `service.<name>.state=<S>` exists with `S` not in `{DORMANT,EXCISED,UNKNOWN}`, else `0`.
  - `shim_dispatch` now handles `start`, `stop`, `restart`, `try-restart`, `reload`, `reload-or-restart`, `daemon-reload`, `daemon-reexec`, `is-active`.

- [ ] **Step 1: Write the failing test**

The test sets `SCHEMA_CTL` to a recording stub script that emits canned `status --kv` output and logs its args. Add to `tests/test_systemctl_shim.c`:

```c
static void make_ctl_stub(const char *active_name) {
    /* writes a fake schema-ctl into the sandbox that logs args to
       $sandbox/ctl.log and prints a --kv line making active_name active */
    char p[400];
    snprintf(p, sizeof p, "%s/schema-ctl", sandbox);
    FILE *f = fopen(p, "w"); assert(f);
    fprintf(f,
        "#!/bin/sh\n"
        "echo \"$@\" >> \"%s/ctl.log\"\n"
        "if [ \"$1\" = status ]; then\n"
        "  echo 'service.%s.state=FULL_TRUST'\n"
        "  echo 'service.sleeper.state=DORMANT'\n"
        "fi\n"
        "exit 0\n", sandbox, active_name);
    fclose(f);
    chmod(p, 0755);
    char ctl[400];
    snprintf(ctl, sizeof ctl, "%s/schema-ctl", sandbox);
    setenv("SCHEMA_CTL", ctl, 1);
}

static int ctl_log_has(const char *needle) {
    char p[400]; snprintf(p, sizeof p, "%s/ctl.log", sandbox);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    char line[400]; int hit = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) { hit = 1; break; }
    fclose(f); return hit;
}

static void test_lifecycle(void) {
    setup_sandbox();
    make_ctl_stub("running");
    /* start passes through to schema-ctl only when a .svc exists */
    char svc[512];
    snprintf(svc, sizeof svc, "%s/svc/running.svc", sandbox);
    FILE *f = fopen(svc, "w"); assert(f); fputs("name=running\n", f); fclose(f);
    assert(run("start", "running.service") == 0);
    assert(ctl_log_has("start running"));
    /* start with no .svc: no crash, exit 0, no passthrough */
    assert(run("start", "ghost.service") == 0);
    /* daemon-reload -> schema-ctl reload */
    assert(run("daemon-reload", NULL) == 0);
    assert(ctl_log_has("reload"));
    /* is-active: active -> 0, dormant -> 3, absent -> 3 */
    assert(run("is-active", "running") == 0);
    assert(run("is-active", "sleeper") == 3);
    assert(run("is-active", "nope") == 3);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: FAIL — `start`/`daemon-reload`/`is-active` not handled; `ctl_log_has`/exit-code asserts fail.

- [ ] **Step 3: Write minimal implementation**

Add to `systemctl_shim.h` (add `#include <sys/wait.h>`):

```c
static int run_ctl(const char *verb, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (name) execlp(shim_ctl(), shim_ctl(), verb, name, (char *)NULL);
        else      execlp(shim_ctl(), shim_ctl(), verb, (char *)NULL);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int ctl_is_active(const char *name) {
    char cmd[512];
    /* read `status --kv` via popen on the resolved ctl program */
    snprintf(cmd, sizeof cmd, "%s status --kv 2>/dev/null", shim_ctl());
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char want[300], line[512];
    snprintf(want, sizeof want, "service.%s.state=", name);
    int active = 0;
    while (fgets(line, sizeof line, p)) {
        char *eq = strstr(line, want);
        if (eq != line) continue;
        char *st = line + strlen(want);
        st[strcspn(st, "\n")] = '\0';
        if (strcmp(st, "DORMANT") && strcmp(st, "EXCISED") && strcmp(st, "UNKNOWN"))
            active = 1;
        break;
    }
    pclose(p);
    return active;
}
```

Extend `shim_dispatch` — add these branches before the final `return 0;`:

```c
    if (strcmp(verb, "daemon-reload") == 0 || strcmp(verb, "daemon-reexec") == 0) {
        run_ctl("reload", NULL);
        return 0;
    }
    if (strcmp(verb, "start") == 0 || strcmp(verb, "stop") == 0 ||
        strcmp(verb, "restart") == 0 || strcmp(verb, "try-restart") == 0 ||
        strcmp(verb, "reload") == 0 || strcmp(verb, "reload-or-restart") == 0) {
        const char *ctlverb = verb;
        if (strcmp(verb, "try-restart") == 0 || strcmp(verb, "reload-or-restart") == 0 ||
            strcmp(verb, "reload") == 0)
            ctlverb = "restart";
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (!unit_supported(argv[i])) continue;
            char name[256];
            strip_service_suffix(argv[i], name, sizeof name);
            if (!svc_exists(name)) continue;
            if (strcmp(verb, "try-restart") == 0 && !ctl_is_active(name)) continue;
            run_ctl(ctlverb, name);
        }
        return 0;
    }
    if (strcmp(verb, "is-active") == 0) {
        for (i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            char name[256];
            strip_service_suffix(argv[i], name, sizeof name);
            return ctl_is_active(name) ? 0 : 3;
        }
        return 3;
    }
```

Note: `reload` maps to `restart` (schema-ctl has no reload-in-place verb; restart is the safe superset for phase 1).

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add systemctl_shim.h tests/test_systemctl_shim.c
git commit -m "feat(systemctl-shim): lifecycle passthrough, daemon-reload, is-active"
```

---

## Task 4: Flag tolerance, `--now`, unknown-verb & empty-argv safety

**Files:**
- Modify: `systemctl_shim.h`
- Modify: `tests/test_systemctl_shim.c`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces: `shim_dispatch` tolerates option flags anywhere in argv (never treats them as units), acts on `--now` for `enable`/`disable`, skips `--user`/`--global` scopes with exit 0, and returns `0` for unknown verbs and empty argv. No new function signatures.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_systemctl_shim.c`:

```c
static int run3(const char *a, const char *b, const char *c) {
    char *argv[5]; int n = 0;
    argv[n++] = (char *)"systemctl";
    argv[n++] = (char *)a;
    if (b) argv[n++] = (char *)b;
    if (c) argv[n++] = (char *)c;
    argv[n] = NULL;
    return shim_dispatch(n, argv);
}

static void test_flags_and_safety(void) {
    setup_sandbox();
    write_unit("foo.service");
    /* flags must not be treated as units; unit still queued */
    assert(run3("enable", "--now", "foo.service") == 0);
    assert(run("is-enabled", "foo") == 0);
    /* --quiet before verb-less noise: unknown verb exits 0 */
    assert(run("frobnicate", "foo") == 0);
    /* empty argv exits 0 */
    char *argv[1] = { (char *)"systemctl" };
    assert(shim_dispatch(1, argv) == 0);
    /* --user scope: skip, exit 0, nothing queued */
    setup_sandbox();
    write_unit("bar.service");
    assert(run3("--user", "enable", "bar.service") == 0);
    assert(run("is-enabled", "bar") == 1);
    /* --now on enable starts the service when a .svc exists */
    make_ctl_stub("baz");
    char svc[512];
    snprintf(svc, sizeof svc, "%s/svc/baz.svc", sandbox);
    FILE *f = fopen(svc, "w"); assert(f); fputs("name=baz\n", f); fclose(f);
    write_unit("baz.service");
    assert(run3("enable", "--now", "baz.service") == 0);
    assert(ctl_log_has("start baz"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: FAIL — `--user` scope not skipped (bar gets queued), `--now` doesn't start baz.

- [ ] **Step 3: Write minimal implementation**

At the top of `shim_dispatch`, after `if (argc < 2) return 0;`, add scope detection and locate the verb past leading global flags:

```c
    int j;
    int now = 0, user_scope = 0;
    for (j = 1; j < argc; j++) {
        if (strcmp(argv[j], "--now") == 0) now = 1;
        else if (strcmp(argv[j], "--user") == 0 || strcmp(argv[j], "--global") == 0)
            user_scope = 1;
    }
    if (user_scope) return 0;
    /* verb = first non-flag token */
    const char *verb = NULL;
    for (j = 1; j < argc; j++) {
        if (argv[j][0] != '-') { verb = argv[j]; break; }
    }
    if (!verb) return 0;
```

Delete the old `const char *verb = argv[1];`. In the `enable`/`preset` branch, after `queue_add(path);`, honor `--now`:

```c
            if (now) {
                char name[256];
                strip_service_suffix(argv[i], name, sizeof name);
                if (svc_exists(name)) run_ctl("start", name);
            }
```

In the `disable` branch, after `unlink(svc);`, honor `--now`:

```c
            if (now) run_ctl("stop", name);
```

The final `return 0;` already covers unknown verbs. Confirm the per-verb unit loops still start at `i = 2`; since flags are skipped inside the loops (`argv[i][0] == '-'`), a flag appearing among units is harmless regardless of position.

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_systemctl_shim.c -o /tmp/schema-test-systemctl && /tmp/schema-test-systemctl`
Expected: PASS.

- [ ] **Step 5: Full C suite regression**

Run: `make test 2>&1 | tail -3`
Expected: whole `test:` recipe passes including the shim line.

- [ ] **Step 6: Commit**

```bash
git add systemctl_shim.h tests/test_systemctl_shim.c
git commit -m "feat(systemctl-shim): flag tolerance, --now, unknown-verb safety"
```

---

## Task 5: RPM packaging — divert scriptlets, %files, spec test, live check

**Files:**
- Modify: `schema-init.spec` (`migrate_bins`, `%files migrate`, scriptlets)
- Create: `tests/test_spec_systemctl_shim.py`
- Create: `tests/verify_systemctl_shim.sh`

**Interfaces:**
- Consumes: the built `schema-systemctl` binary (Tasks 1–4) installed to `%{_bindir}` by `install-migrate`.
- Produces: RPM diversion of `/usr/bin/systemctl`, verified by a spec test.

- [ ] **Step 1: Write the failing spec test**

Create `tests/test_spec_systemctl_shim.py`:

```python
#!/usr/bin/env python3
"""schema-systemctl shim packaging spec tests — script-style."""
import os, sys
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = open(os.path.join(REPO, "schema-init.spec")).read()

results = []
def check(name, cond):
    results.append(bool(cond)); print(("  ok  " if cond else "  FAIL ") + name)

check("shim in migrate_bins", "schema-systemctl" in SPEC and "migrate_bins" in SPEC)
check("shim shipped in -migrate %files", "%{_bindir}/schema-systemctl" in SPEC)
check("post diverts systemctl", "systemctl.real" in SPEC and "%post migrate" in SPEC)
check("post is idempotent", "! -L /usr/bin/systemctl" in SPEC)
check("postun restore gated on removal", "-eq 0" in SPEC and "%postun migrate" in SPEC)
check("transfiletrigger re-diverts", "%transfiletriggerin migrate -- /usr/bin/systemctl" in SPEC)

print("PASS" if all(results) else "FAIL"); sys.exit(0 if all(results) else 1)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/test_spec_systemctl_shim.py`
Expected: FAIL — none of the shim wiring is in the spec yet.

- [ ] **Step 3: Edit `schema-init.spec`**

Add `schema-systemctl` to the `%global migrate_bins` line. Add `%{_bindir}/schema-systemctl` to `%files migrate`. Replace the existing `%post migrate` / `%postun migrate` blocks with the ones below (keeping the existing schema-udev.ship-md5 lines), and add the `%transfiletriggerin`:

```spec
%post migrate
md5sum %{_bindir}/schema-udev | cut -d' ' -f1 > %{_sysconfdir}/schema-init/schema-udev.ship-md5
if alternatives --display systemctl >/dev/null 2>&1; then
    alternatives --install /usr/bin/systemctl systemctl %{_bindir}/schema-systemctl 100
else
    if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
        mv /usr/bin/systemctl /usr/bin/systemctl.real
    fi
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi

%postun migrate
if [ $1 -eq 0 ]; then
    rm -f %{_sysconfdir}/schema-init/schema-udev.ship-md5
    if alternatives --display systemctl >/dev/null 2>&1; then
        alternatives --remove systemctl %{_bindir}/schema-systemctl
    elif [ -f /usr/bin/systemctl.real ]; then
        rm -f /usr/bin/systemctl
        mv /usr/bin/systemctl.real /usr/bin/systemctl
    fi
fi

%transfiletriggerin migrate -- /usr/bin/systemctl
if [ ! -L /usr/bin/systemctl ] && [ -f /usr/bin/systemctl ]; then
    mv -f /usr/bin/systemctl /usr/bin/systemctl.real
    ln -sf %{_bindir}/schema-systemctl /usr/bin/systemctl
fi
```

- [ ] **Step 4: Run spec test + rpmspec parse to verify pass**

Run: `python3 tests/test_spec_systemctl_shim.py && rpmspec -q --rpms schema-init.spec >/dev/null && echo "rpmspec ok"`
Expected: `PASS` then `rpmspec ok` (spec still parses).

- [ ] **Step 5: Create the live exit-code check**

Create `tests/verify_systemctl_shim.sh` (chmod +x). Mirrors the repo's `verify_*_live.sh` style — builds the binary, drives it in a temp sandbox, asserts the exit-code contract end-to-end:

```sh
#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make schema-systemctl >/dev/null
BIN=$PWD/schema-systemctl
SB=$(mktemp -d)
export SCHEMA_STATE_DIR=$SB/state SCHEMA_SVC_DIR=$SB/svc SCHEMA_UNIT_DIR=$SB/units
mkdir -p "$SCHEMA_STATE_DIR" "$SCHEMA_SVC_DIR" "$SCHEMA_UNIT_DIR"
: > "$SCHEMA_UNIT_DIR/foo.service"

"$BIN" enable foo.service
grep -q foo.service "$SCHEMA_STATE_DIR/pending.list" || { echo "FAIL: not queued"; exit 1; }
"$BIN" is-enabled foo;                 [ $? -eq 0 ] || { echo "FAIL is-enabled"; exit 1; }
"$BIN" is-active foo;   rc=$?;         [ "$rc" -eq 3 ] || { echo "FAIL is-active rc=$rc"; exit 1; }
"$BIN" frobnicate foo;                 [ $? -eq 0 ] || { echo "FAIL unknown-verb"; exit 1; }
"$BIN" disable foo
"$BIN" is-enabled foo; rc=$?;          [ "$rc" -eq 1 ] || { echo "FAIL disable rc=$rc"; exit 1; }
rm -rf "$SB"
echo "verify_systemctl_shim: PASS"
```

Run: `sh tests/verify_systemctl_shim.sh`
Expected: `verify_systemctl_shim: PASS`.

- [ ] **Step 6: Commit**

```bash
git add schema-init.spec tests/test_spec_systemctl_shim.py tests/verify_systemctl_shim.sh
git commit -m "feat(systemctl-shim): RPM divert scriptlets, %files, spec + live tests"
```

---

## Self-Review

**Spec coverage:**
- Purpose / make scriptlets succeed → Tasks 2–4 (exit-0 contract) + Task 5 (divert). ✔
- Verb contract table → Task 2 (enable/preset/disable/is-enabled), Task 3 (lifecycle/daemon-reload/is-active), Task 4 (--now, unknown verb). ✔ `mask`/`unmask` record-only → falls through to `return 0` (unknown-verb path); acceptable per spec "phase 1: log + exit 0" (no explicit log, but exit 0 satisfied). `status` → falls through to `return 0`; spec calls it best-effort — acceptable phase-1 no-op. **Noted deviation:** `mask`/`unmask`/`status` are exit-0 no-ops, not logged. Consistent with the exit-code contract; log lines can be added later without interface change.
- Queue (dedup/remove/path/hand-off) → Task 2. ✔
- Unit normalization + type skip → Task 1 + used throughout. ✔
- Flags tolerated/ignored → Task 4. ✔
- schema-ctl coupling + degraded mode → Task 3 (`run_ctl`/`ctl_is_active` return benign on spawn failure; no socket → status empty → is-active 3, lifecycle no-ops). ✔
- Shadowing %post/%postun/%transfiletriggerin (idempotent, removal-gated, self-healing, alternatives guard) → Task 5. ✔
- Testing 12-case list → distributed across Tasks 1–4 C tests + Task 5 live script. ✔
- Files touched list → matches Task file headers. ✔

**Placeholder scan:** none — every step carries real code or an exact command.

**Type consistency:** `strip_service_suffix`, `unit_supported`, `resolve_unit_path`, `queue_add/remove/contains`, `svc_exists`, `run_ctl`, `ctl_is_active`, `shim_dispatch` — signatures identical everywhere referenced. `verb` becomes a scanned local in Task 4 (replacing Task 2's `argv[1]`); Task 4 Step 3 explicitly deletes the old declaration to avoid a redefinition. ✔

**One carried risk for the executor:** Task 4 introduces `const char *verb` computed by scan and says to delete the Task-2 `const char *verb = argv[1];`. If executed out of order, the compiler catches the duplicate — not silent.
