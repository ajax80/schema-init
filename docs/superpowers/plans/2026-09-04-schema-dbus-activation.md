# schema-dbus service activation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the schema-dbus broker on-demand service activation — a method call to an unowned-but-activatable name (or an explicit `StartServiceByName`) spawns the service, holds the triggering message, and delivers it once the name is claimed.

**Architecture:** A new pure-logic header `sdbus_activate.h` owns the service-file table and the pending-activation state machine (no I/O, no fork, no dbus — unit-testable with C asserts, in the style of `sdbus_reply.h`). The main loop in `schema-dbus.c` owns the side effects: fork+exec with uid drop, a `signalfd(SIGCHLD)` in the epoll set, the spawn-timeout deadline folded into `epoll_wait`, and the four hook points. PID 1 is untouched.

**Tech Stack:** C11, libdbus (wire codec only), epoll, signalfd, POSIX fork/exec/setuid. Build via the existing Makefile; tests are freestanding C `main()` programs run from the `test:` target.

**Spec:** `docs/superpowers/specs/2026-09-04-schema-dbus-activation-design.md`

## Global Constraints

- Behavioral parity with stock dbus-daemon; no per-caller activation policy gate; no audit log.
- Always drop privilege to the file's `User=` (default `root`) before `execv`; sanitize env; rely on CLOEXEC for fd hygiene.
- Skip service files whose `Exec=` is `/bin/false` — they stay non-activatable (keep erroring `ServiceUnknown`).
- Spawn timeout: `25000` ms (stock's value). Error names come from `dbus/dbus-protocol.h` macros: `DBUS_ERROR_TIMED_OUT`, `DBUS_ERROR_SPAWN_FAILED`, `DBUS_ERROR_SPAWN_CHILD_EXITED`. Start-reply codes from `dbus/dbus-shared.h`: `DBUS_START_REPLY_SUCCESS` (1), `DBUS_START_REPLY_ALREADY_RUNNING` (2).
- No changes to policy, names, route, reply, match, conn, auth, wire, codec modules or `init.c`.
- Commit messages end with the two trailers already used on this branch (`Co-Authored-By: Claude Opus 4.8 …` + `Claude-Session: …`).
- Never reboot to deploy without a green `make test` **and** a green `schema-vmtest` first; deploy/reboot is Jonathan's call, not the executor's.

---

### Task 1: Service-file table (parse)

**Files:**
- Create: `sdbus_activate.h`
- Test: `tests/test_sdbus_activate.c`
- Modify: `Makefile:156` area (append a test line after the `test_sdbus_reply` line)

**Interfaces:**
- Produces:
  - `typedef struct { char *name; char **argv; char *user; } sdbus_svc_ent;`
  - `typedef struct { sdbus_svc_ent *v; int n; } sdbus_svctab;`
  - `sdbus_svctab *sdbus_svctab_parse_dir(const char *dir);` — glob `<dir>/*.service`, parse `Name=`/`Exec=`/`User=`; skip entries with no `Name=`, no real `Exec=`, or `Exec=/bin/false`. `argv` is NULL-terminated (Exec split on spaces). `user` defaults to `"root"`.
  - `const sdbus_svc_ent *sdbus_svctab_find(sdbus_svctab *t, const char *name);`
  - `void sdbus_svctab_free(sdbus_svctab *t);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_sdbus_activate.c`:

```c
#include "../sdbus_activate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* write a .service file into dir */
static void put(const char *dir, const char *fn, const char *body) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, fn);
    FILE *f = fopen(p, "w"); assert(f); fputs(body, f); fclose(f);
}

static void test_parse(void) {
    char dir[] = "/tmp/sdbus-act-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "real.service",
        "[D-BUS Service]\nName=com.example.Real\nExec=/usr/libexec/realsvc --arg\nUser=root\n");
    put(dir, "nouser.service",
        "[D-BUS Service]\nName=com.example.NoUser\nExec=/usr/libexec/nu\n");
    put(dir, "false.service",
        "[D-BUS Service]\nName=com.example.Systemd\nExec=/bin/false\nSystemdService=x.service\n");
    put(dir, "noname.service", "[D-BUS Service]\nExec=/usr/libexec/x\n");

    sdbus_svctab *t = sdbus_svctab_parse_dir(dir);
    assert(t);

    const sdbus_svc_ent *r = sdbus_svctab_find(t, "com.example.Real");
    assert(r);
    assert(!strcmp(r->argv[0], "/usr/libexec/realsvc"));
    assert(!strcmp(r->argv[1], "--arg"));
    assert(r->argv[2] == NULL);
    assert(!strcmp(r->user, "root"));

    const sdbus_svc_ent *n = sdbus_svctab_find(t, "com.example.NoUser");
    assert(n && !strcmp(n->user, "root"));           /* default */

    assert(sdbus_svctab_find(t, "com.example.Systemd") == NULL);   /* /bin/false skipped */
    assert(sdbus_svctab_find(t, "com.example.Missing") == NULL);   /* absent */
    assert(t->n == 2);                                /* real + nouser only */

    sdbus_svctab_free(t);
    /* cleanup */
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_parse OK\n");
}

int main(void) {
    test_parse();
    printf("all sdbus_activate tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall tests/test_sdbus_activate.c -o /tmp/t-act`
Expected: FAIL — `sdbus_activate.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `sdbus_activate.h` with header guard and the parse half:

```c
#ifndef SDBUS_ACTIVATE_H
#define SDBUS_ACTIVATE_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name; char **argv; char *user; } sdbus_svc_ent;
typedef struct { sdbus_svc_ent *v; int n; } sdbus_svctab;

/* split "a b c" into a NULL-terminated argv (whitespace-delimited, no quoting —
   dbus service Exec lines are plain paths + flags). */
static inline char **sdbus__split_argv(const char *s) {
    int cap = 4, n = 0;
    char **a = malloc(cap * sizeof *a);
    char *dup = strdup(s), *save = NULL;
    for (char *tok = strtok_r(dup, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (n + 1 >= cap) { cap *= 2; a = realloc(a, cap * sizeof *a); }
        a[n++] = strdup(tok);
    }
    a[n] = NULL;
    free(dup);
    return a;
}

static inline void sdbus__svc_add(sdbus_svctab *t, const char *name,
                                  const char *exec, const char *user) {
    t->v = realloc(t->v, (t->n + 1) * sizeof *t->v);
    sdbus_svc_ent *e = &t->v[t->n++];
    e->name = strdup(name);
    e->argv = sdbus__split_argv(exec);
    e->user = strdup(user && *user ? user : "root");
}

static inline sdbus_svctab *sdbus_svctab_parse_dir(const char *dir) {
    sdbus_svctab *t = calloc(1, sizeof *t);
    DIR *d = opendir(dir);
    if (!d) return t;                       /* empty table, not NULL */
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".service")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[2048], name[512] = "", exec[1536] = "", user[256] = "";
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strncmp(line, "Name=", 5))      snprintf(name, sizeof name, "%s", line + 5);
            else if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
            else if (!strncmp(line, "User=", 5)) snprintf(user, sizeof user, "%s", line + 5);
        }
        fclose(f);
        if (!name[0] || !exec[0]) continue;                 /* need both */
        /* skip Exec=/bin/false (systemd-only activatables) */
        if (!strcmp(exec, "/bin/false") || !strcmp(exec, "/usr/bin/false")) continue;
        sdbus__svc_add(t, name, exec, user);
    }
    closedir(d);
    return t;
}

static inline const sdbus_svc_ent *sdbus_svctab_find(sdbus_svctab *t, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].name, name)) return &t->v[i];
    return NULL;
}

static inline void sdbus_svctab_free(sdbus_svctab *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        free(t->v[i].name); free(t->v[i].user);
        for (char **a = t->v[i].argv; a && *a; a++) free(*a);
        free(t->v[i].argv);
    }
    free(t->v); free(t);
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall tests/test_sdbus_activate.c -o /tmp/t-act && /tmp/t-act`
Expected: PASS — prints `test_parse OK` then `all sdbus_activate tests passed`.

- [ ] **Step 5: Wire into the Makefile test target**

Add after `Makefile:156` (`test_sdbus_reply` line), matching the existing tab-indented style:

```make
	$(CC) $(CFLAGS) tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate
```

- [ ] **Step 6: Commit**

```bash
git add sdbus_activate.h tests/test_sdbus_activate.c Makefile
git commit -m "feat(sdbus): service-file activation table parser"
```

---

### Task 2: Pending-activation state machine

**Files:**
- Modify: `sdbus_activate.h` (append the pending-table half, before `#endif`)
- Modify: `tests/test_sdbus_activate.c` (add table tests, call them from `main`)

**Interfaces:**
- Consumes: nothing from Task 1 (independent structs in the same header).
- Produces:
  - `typedef enum { SDBUS_HELD_IMPLICIT, SDBUS_HELD_EXPLICIT } sdbus_held_kind;`
  - `typedef struct { unsigned char *bytes; int len; int *fds; int nfds; int caller_id; uint32_t serial; int expects_reply; sdbus_held_kind kind; } sdbus_held_msg;`
  - `typedef struct { char *name; int child_pid; long deadline_ms; sdbus_held_msg *held; int n_held; } sdbus_pending_act;`
  - `typedef struct { sdbus_pending_act *v; int n; } sdbus_acts;`
  - `sdbus_acts *sdbus_acts_new(void);`
  - `sdbus_pending_act *sdbus_acts_find(sdbus_acts *a, const char *name);`
  - `sdbus_pending_act *sdbus_acts_begin(sdbus_acts *a, const char *name, int pid, long deadline_ms);` — create empty entry for a name that is now SPAWNING.
  - `void sdbus_acts_hold(sdbus_pending_act *e, const sdbus_held_msg *m);` — deep-copies `bytes`/`fds`.
  - `int sdbus_acts_take(sdbus_acts *a, const char *name, sdbus_held_msg **out, int *n);` — remove the entry for `name`, hand its held array to the caller (caller frees each `bytes`/`fds` then `free(out)`). Returns 1 if found, 0 if not.
  - `sdbus_pending_act *sdbus_acts_by_pid(sdbus_acts *a, int pid);`
  - `long sdbus_acts_next_deadline(sdbus_acts *a);` — earliest `deadline_ms`, or -1 if none.
  - `int sdbus_acts_reap_expired(sdbus_acts *a, long now_ms, sdbus_held_msg **out, int *n);` — same take-semantics for the first expired entry; returns 1 if one was reaped.
  - `void sdbus_acts_free(sdbus_acts *a);`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sdbus_activate.c` and call `test_pending()` from `main` before the final print:

```c
static sdbus_held_msg mk(int caller, uint32_t serial, sdbus_held_kind k) {
    sdbus_held_msg m = {0};
    m.bytes = (unsigned char *)strdup("wire"); m.len = 4;
    m.caller_id = caller; m.serial = serial; m.expects_reply = 1; m.kind = k;
    return m;
}

static void test_pending(void) {
    sdbus_acts *a = sdbus_acts_new();
    assert(sdbus_acts_find(a, "com.x") == NULL);
    assert(sdbus_acts_next_deadline(a) == -1);

    sdbus_pending_act *e = sdbus_acts_begin(a, "com.x", 4242, 1000);
    assert(e && e->child_pid == 4242 && e->n_held == 0);
    assert(sdbus_acts_find(a, "com.x") == e);
    assert(sdbus_acts_by_pid(a, 4242) == e);
    assert(sdbus_acts_next_deadline(a) == 1000);

    sdbus_held_msg m1 = mk(10, 1, SDBUS_HELD_IMPLICIT);
    sdbus_held_msg m2 = mk(11, 2, SDBUS_HELD_EXPLICIT);
    sdbus_acts_hold(e, &m1); sdbus_acts_hold(e, &m2);
    free(m1.bytes); free(m2.bytes);                 /* hold deep-copies */
    assert(sdbus_acts_find(a, "com.x")->n_held == 2);

    /* take removes the entry and hands back the held array */
    sdbus_held_msg *out = NULL; int n = 0;
    assert(sdbus_acts_take(a, "com.x", &out, &n) == 1);
    assert(n == 2 && out[0].caller_id == 10 && out[1].kind == SDBUS_HELD_EXPLICIT);
    assert(sdbus_acts_find(a, "com.x") == NULL);     /* gone */
    for (int i = 0; i < n; i++) { free(out[i].bytes); free(out[i].fds); }
    free(out);

    /* reap_expired picks the entry whose deadline has passed */
    sdbus_acts_begin(a, "com.y", 5, 500);
    sdbus_acts_begin(a, "com.z", 6, 3000);
    sdbus_held_msg *o2 = NULL; int n2 = 0;
    assert(sdbus_acts_reap_expired(a, 600, &o2, &n2) == 1);   /* com.y expired */
    assert(sdbus_acts_find(a, "com.y") == NULL);
    assert(sdbus_acts_find(a, "com.z") != NULL);              /* not yet */
    free(o2);
    assert(sdbus_acts_reap_expired(a, 600, &o2, &n2) == 0);   /* none left expired */

    sdbus_acts_free(a);
    printf("test_pending OK\n");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -std=c11 -Wall tests/test_sdbus_activate.c -o /tmp/t-act`
Expected: FAIL — implicit declarations / unknown types (`sdbus_acts` etc.).

- [ ] **Step 3: Write minimal implementation**

Append to `sdbus_activate.h` before `#endif` (add `#include <stdint.h>` at the top of the file):

```c
typedef enum { SDBUS_HELD_IMPLICIT, SDBUS_HELD_EXPLICIT } sdbus_held_kind;
typedef struct {
    unsigned char *bytes; int len;      /* captured wire message (implicit) */
    int *fds; int nfds;                 /* its passed fds (implicit) */
    int caller_id; uint32_t serial; int expects_reply;
    sdbus_held_kind kind;
} sdbus_held_msg;
typedef struct {
    char *name; int child_pid; long deadline_ms;
    sdbus_held_msg *held; int n_held;
} sdbus_pending_act;
typedef struct { sdbus_pending_act *v; int n; } sdbus_acts;

static inline sdbus_acts *sdbus_acts_new(void) { return calloc(1, sizeof(sdbus_acts)); }

static inline sdbus_pending_act *sdbus_acts_find(sdbus_acts *a, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].name, name)) return &a->v[i];
    return NULL;
}

static inline sdbus_pending_act *sdbus_acts_by_pid(sdbus_acts *a, int pid) {
    for (int i = 0; i < a->n; i++) if (a->v[i].child_pid == pid) return &a->v[i];
    return NULL;
}

static inline sdbus_pending_act *sdbus_acts_begin(sdbus_acts *a, const char *name,
                                                  int pid, long deadline_ms) {
    a->v = realloc(a->v, (a->n + 1) * sizeof *a->v);
    sdbus_pending_act *e = &a->v[a->n++];
    memset(e, 0, sizeof *e);
    e->name = strdup(name); e->child_pid = pid; e->deadline_ms = deadline_ms;
    return e;
}

static inline void sdbus_acts_hold(sdbus_pending_act *e, const sdbus_held_msg *m) {
    e->held = realloc(e->held, (e->n_held + 1) * sizeof *e->held);
    sdbus_held_msg *d = &e->held[e->n_held++];
    *d = *m;
    if (m->len > 0 && m->bytes) { d->bytes = malloc(m->len); memcpy(d->bytes, m->bytes, m->len); }
    else { d->bytes = NULL; d->len = 0; }
    if (m->nfds > 0 && m->fds) { d->fds = malloc(m->nfds * sizeof(int)); memcpy(d->fds, m->fds, m->nfds * sizeof(int)); }
    else { d->fds = NULL; d->nfds = 0; }
}

/* remove entry at index i, handing its held[] array to *out (ownership transfers). */
static inline void sdbus__acts_pop(sdbus_acts *a, int i, sdbus_held_msg **out, int *n) {
    *out = a->v[i].held; *n = a->v[i].n_held;
    free(a->v[i].name);
    a->v[i] = a->v[--a->n];             /* swap-remove */
}

static inline int sdbus_acts_take(sdbus_acts *a, const char *name,
                                  sdbus_held_msg **out, int *n) {
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].name, name)) { sdbus__acts_pop(a, i, out, n); return 1; }
    *out = NULL; *n = 0; return 0;
}

static inline long sdbus_acts_next_deadline(sdbus_acts *a) {
    long best = -1;
    for (int i = 0; i < a->n; i++)
        if (best < 0 || a->v[i].deadline_ms < best) best = a->v[i].deadline_ms;
    return best;
}

static inline int sdbus_acts_reap_expired(sdbus_acts *a, long now_ms,
                                          sdbus_held_msg **out, int *n) {
    for (int i = 0; i < a->n; i++)
        if (a->v[i].deadline_ms <= now_ms) { sdbus__acts_pop(a, i, out, n); return 1; }
    *out = NULL; *n = 0; return 0;
}

static inline void sdbus_acts_free(sdbus_acts *a) {
    if (!a) return;
    for (int i = 0; i < a->n; i++) {
        free(a->v[i].name);
        for (int j = 0; j < a->v[i].n_held; j++) { free(a->v[i].held[j].bytes); free(a->v[i].held[j].fds); }
        free(a->v[i].held);
    }
    free(a->v); free(a);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cc -std=c11 -Wall tests/test_sdbus_activate.c -o /tmp/t-act && /tmp/t-act`
Expected: PASS — `test_parse OK`, `test_pending OK`, `all sdbus_activate tests passed`.

- [ ] **Step 5: Commit**

```bash
git add sdbus_activate.h tests/test_sdbus_activate.c
git commit -m "feat(sdbus): pending-activation state machine + tests"
```

---

### Task 3: Broker wiring — include header, parse table at startup, build dep

**Files:**
- Modify: `schema-dbus.c` (include + globals + startup parse)
- Modify: `Makefile:65` (add `sdbus_activate.h` to the `schema-dbus` prerequisite list)

**Interfaces:**
- Consumes: `sdbus_svctab_parse_dir`, `sdbus_acts_new` (Tasks 1-2).
- Produces: file-scope globals `static sdbus_svctab *g_svctab;` and `static sdbus_acts *g_acts;` used by later tasks; `#define SDBUS_SVC_DIR "/usr/share/dbus-1/system-services"`.

- [ ] **Step 1: Add the include** — after `schema-dbus.c:27` (`#include "sdbus_route.h"`):

```c
#include "sdbus_activate.h"
```

- [ ] **Step 2: Add globals + dir macro** — after `schema-dbus.c:39` (`static dbus_uint32_t g_bcast_serial;`):

```c
static sdbus_svctab  *g_svctab;
static sdbus_acts    *g_acts;
#define SDBUS_SVC_DIR "/usr/share/dbus-1/system-services"
#define SDBUS_SPAWN_TIMEOUT_MS 25000
```

- [ ] **Step 3: Parse at startup** — after `schema-dbus.c:408` (`g_replies = sdbus_replies_new();`):

```c
    g_svctab = sdbus_svctab_parse_dir(SDBUS_SVC_DIR);
    g_acts = sdbus_acts_new();
    fprintf(stderr, "schema-dbus: %d activatable services\n", g_svctab->n);
```

- [ ] **Step 4: Add build dependency** — on `Makefile:65`, append `sdbus_activate.h` to the `schema-dbus:` prerequisite list.

- [ ] **Step 5: Verify it builds**

Run: `make schema-dbus`
Expected: compiles clean, no warnings; running `./schema-dbus --system` on a scratch socket prints the "N activatable services" line. (Do NOT install/replace the live bus.)

- [ ] **Step 6: Commit**

```bash
git add schema-dbus.c Makefile
git commit -m "feat(sdbus): load activatable-service table at broker startup"
```

---

### Task 4: Spawn helper (fork + uid-drop + exec)

**Files:**
- Modify: `schema-dbus.c` (add `spawn_service`, plus signal/pwd includes)

**Interfaces:**
- Consumes: `sdbus_svc_ent` (Task 1).
- Produces: `static pid_t spawn_service(const sdbus_svc_ent *e, const char *bus_addr);` — fork+exec; returns child pid, or `-1` on fork failure. Also `static char g_bus_addr[256];` holding the `unix:path=<socket>` string, set in `main`.

- [ ] **Step 1: Add includes** — with the other `#include`s at the top of `schema-dbus.c`:

```c
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
```

- [ ] **Step 2: Implement `spawn_service`** — near the top of the helpers section (e.g. after `make_listen_socket`, before `main`):

```c
/* fork+exec an activatable service, dropping to its User= before exec. Returns
   the child pid, or -1 if fork failed. Matches stock dbus-daemon: clean env with
   DBUS_STARTER_*; all broker fds are CLOEXEC so exec closes them. */
static pid_t spawn_service(const sdbus_svc_ent *e, const char *bus_addr) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) return pid;

    /* --- child --- */
    setsid();
    struct passwd *pw = getpwnam(e->user);
    if (pw && pw->pw_uid != 0) {
        if (initgroups(e->user, pw->pw_gid) != 0) _exit(127);
        if (setgid(pw->pw_gid) != 0) _exit(127);
        if (setuid(pw->pw_uid) != 0) _exit(127);
    }
    char starter[320];
    snprintf(starter, sizeof starter, "DBUS_STARTER_ADDRESS=%s", bus_addr);
    char *env[] = {
        (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin",
        starter,
        (char *)"DBUS_STARTER_BUS_TYPE=system",
        NULL
    };
    execve(e->argv[0], e->argv, env);
    _exit(127);                         /* exec failed */
}
```

- [ ] **Step 3: Set `g_bus_addr` in main** — after the socket path is known (`schema-dbus.c:384`, right after `if (!sock) sock = DEFAULT_SOCKET;`):

```c
    static char g_bus_addr[256];
    snprintf(g_bus_addr, sizeof g_bus_addr, "unix:path=%s", sock);
```

Move the `g_bus_addr` declaration to file scope (with the other globals in Task 3) if later tasks in the same session need it; for now local-to-main is fine since only `spawn_service` callers in `main`'s loop use it. (Note for executor: Task 5 needs it at file scope — declare `static char g_bus_addr[256];` among the globals and assign it here without the `static` keyword.)

- [ ] **Step 4: Verify it builds**

Run: `make schema-dbus`
Expected: compiles clean, no warnings. (No unit test — the fork/uid path is exercised by the live smoke test in Task 9.)

- [ ] **Step 5: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): spawn helper with User= drop + DBUS_STARTER env"
```

---

### Task 5: Implicit trigger — hold the message + spawn

**Files:**
- Modify: `schema-dbus.c` (`handle_message` synth branch + a helper)

**Interfaces:**
- Consumes: `g_svctab`, `g_acts`, `spawn_service`, `g_bus_addr` (Tasks 3-4); `sdbus_acts_find`/`_begin`/`_hold` (Task 2).
- Produces: `static void activate_or_hold(sdbus_conn *c, sdbus_wire_msg *w, const unsigned char *raw, int rawlen, int nfds, int *transferred);` returning 1 if it handled (held) the message, 0 if the name is not activatable (caller should fall through to ServiceUnknown).

- [ ] **Step 1: Implement `activate_or_hold`** — above `handle_message`:

```c
/* If w->destination is an activatable name with no current owner, capture this
   message (bytes + fds + caller + serial), spawn the service if not already
   spawning, and return 1 (message is held, not answered). Return 0 if the name
   is not activatable (caller emits ServiceUnknown as before). */
static int activate_or_hold(sdbus_conn *c, sdbus_wire_msg *w,
                            const unsigned char *raw, int rawlen,
                            int nfds, int *transferred) {
    if (!w->destination) return 0;
    const sdbus_svc_ent *e = sdbus_svctab_find(g_svctab, w->destination);
    if (!e) return 0;

    sdbus_held_msg m = {0};
    /* capture the reforwarded bytes (sender stamped), so release re-injects a
       ready-to-route message. */
    unsigned char *fwd = NULL; int fwlen = 0;
    if (sdbus_wire_reforward(raw, w, c->unique, &fwd, &fwlen) != 0) return 0;
    m.bytes = fwd; m.len = fwlen;
    m.caller_id = c->id; m.serial = w->serial;
    m.expects_reply = !(w->flags & SDBUS_FLAG_NO_REPLY);
    m.kind = SDBUS_HELD_IMPLICIT;
    if (nfds > 0) { m.fds = c->pending_fds; m.nfds = nfds; *transferred = 1; }

    sdbus_pending_act *pa = sdbus_acts_find(g_acts, w->destination);
    if (!pa) {
        pid_t pid = spawn_service(e, g_bus_addr);
        if (pid < 0) { free(fwd); return 0; }        /* fork failed -> ServiceUnknown path */
        pa = sdbus_acts_begin(g_acts, w->destination, pid, sdbus__now_ms() + SDBUS_SPAWN_TIMEOUT_MS);
    }
    sdbus_acts_hold(pa, &m);        /* deep-copies bytes + fds */
    free(fwd);
    return 1;
}
```

- [ ] **Step 2: Hook the synth branch** — replace `schema-dbus.c:249-250`:

```c
    } else if (synth) {
        synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "name has no owner");
```

with:

```c
    } else if (synth) {
        if (!activate_or_hold(c, w, raw, rawlen, nfds, &transferred))
            synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "name has no owner");
```

(`transferred` is already declared at `schema-dbus.c:246`; when `activate_or_hold` sets it, the existing `consume_msg_fds(c, nfds, transferred)` at the end of `handle_message` correctly leaves the fds for the held copy — but note the held copy dup'd them via `sdbus_acts_hold`, so set `*transferred = 1` there means the original fds are NOT closed by consume; they remain owned by `c->pending_fds` and are consumed normally. See Step 3.)

- [ ] **Step 3: Fix fd ownership** — the held message deep-copies fd *numbers*, not duplicated descriptors. To avoid double-close, do NOT set `*transferred` for the held path; instead let `consume_msg_fds` close the originals and have the held copy `dup()` them. Change the fd-capture line in `activate_or_hold` (Step 1) to dup:

```c
    if (nfds > 0) {
        m.fds = malloc(nfds * sizeof(int));
        for (int i = 0; i < nfds; i++) m.fds[i] = dup(c->pending_fds[i]);
        m.nfds = nfds;
    }
```

and remove the `*transferred = 1;` assignment and the `int *transferred` use for this path (drop that param — signature becomes `activate_or_hold(sdbus_conn *c, sdbus_wire_msg *w, const unsigned char *raw, int rawlen, int nfds)`). `sdbus_acts_hold` then copies the dup'd fd array; free `m.fds` after the hold call (hold deep-copies). Update the Step 2 call accordingly (drop `&transferred`).

- [ ] **Step 4: Verify it builds**

Run: `make schema-dbus`
Expected: clean build. Behavior verified in Task 9 (live) — a cold call to an activatable name no longer returns ServiceUnknown immediately (it hangs pending until release/timeout, which arrive in Tasks 6-8).

- [ ] **Step 5: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): hold + spawn on implicit activation trigger"
```

---

### Task 6: SIGCHLD reaping + fail-on-child-death

**Files:**
- Modify: `schema-dbus.c` (`main`: block SIGCHLD, signalfd in epoll, reap block)

**Interfaces:**
- Consumes: `g_acts`, `sdbus_acts_by_pid`, `sdbus_acts_take`, `send_no_reply_to` / `synth_error_wire` (existing).
- Produces: a `signalfd`-driven reaper; a helper `static void fail_held(sdbus_held_msg *held, int n, const char *errname, const char *text);`.

- [ ] **Step 1: Add `fail_held` helper** — after `send_no_reply_to`:

```c
/* reply an error to each held caller of an activation that failed, and free the
   held array (bytes/fds owned by the array). */
static void fail_held(sdbus_held_msg *held, int n, const char *errname, const char *text) {
    for (int i = 0; i < n; i++) {
        sdbus_conn *caller = conn_by_id(held[i].caller_id);
        if (caller && held[i].expects_reply) {
            synth_error_wire(caller, held[i].serial, errname, text);
            ep_update(caller);
        }
        for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        free(held[i].bytes); free(held[i].fds);
    }
    free(held);
}
```

- [ ] **Step 2: Block SIGCHLD + create signalfd** — in `main`, before `g_epfd = epoll_create1(...)` (`schema-dbus.c:413`):

```c
    sigset_t scmask;
    sigemptyset(&scmask);
    sigaddset(&scmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &scmask, NULL);
    int sigfd = signalfd(-1, &scmask, SFD_NONBLOCK | SFD_CLOEXEC);
```

- [ ] **Step 3: Register signalfd in epoll** — after the listen fd is added (`schema-dbus.c:416`). Use a distinct sentinel: define near the top of `main` `static int sigfd_marker;` and register `ev.data.ptr = &sigfd_marker;`:

```c
    struct epoll_event sev = {0};
    sev.events = EPOLLIN; sev.data.ptr = &sigfd_marker;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, sigfd, &sev);
```

- [ ] **Step 4: Handle the signalfd event** — in the event loop, add a branch alongside the listen-fd check (`schema-dbus.c:432`), before the `sdbus_conn *c = evs[i].data.ptr;` line:

```c
            if (evs[i].data.ptr == &sigfd_marker) {
                struct signalfd_siginfo si;
                while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) { }  /* drain */
                int st; pid_t p;
                while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
                    sdbus_pending_act *pa = sdbus_acts_by_pid(g_acts, (int)p);
                    if (!pa) continue;               /* exited after claiming its name: normal */
                    sdbus_held_msg *held = NULL; int nh = 0;
                    sdbus_acts_take(g_acts, pa->name, &held, &nh);
                    fail_held(held, nh, DBUS_ERROR_SPAWN_CHILD_EXITED,
                              "Activated service exited before acquiring its name");
                }
                continue;
            }
```

- [ ] **Step 5: Verify it builds**

Run: `make schema-dbus`
Expected: clean build. (Behavior: a spawned binary that exits without claiming its name now sends `Spawn.ChildExited` to held callers — verified live in Task 9 with a deliberately-broken service, optional.)

- [ ] **Step 6: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): SIGCHLD reaper, fail held callers on early child exit"
```

---

### Task 7: Release held messages on name acquisition

**Files:**
- Modify: `schema-dbus.c` (`broadcast_transitions` name-acquire branch)

**Interfaces:**
- Consumes: `g_acts`, `sdbus_acts_take` (Task 2), `sdbus_route_targets` (existing), `sdbus_conn_enqueue` (existing).
- Produces: nothing new; extends `broadcast_transitions`.

- [ ] **Step 1: Add a release helper** — above `broadcast_transitions`:

```c
/* a name just got an owner: deliver every message held for it. IMPLICIT ones are
   re-routed as a fresh send (records a pending reply so the answer routes back);
   EXPLICIT StartServiceByName callers get a SUCCESS reply. */
static void release_activation(const char *name) {
    sdbus_held_msg *held = NULL; int n = 0;
    if (!sdbus_acts_take(g_acts, name, &held, &n)) return;
    for (int i = 0; i < n; i++) {
        sdbus_conn *caller = conn_by_id(held[i].caller_id);
        if (held[i].kind == SDBUS_HELD_EXPLICIT) {
            if (caller) {
                DBusMessage *r = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
                dbus_message_set_reply_serial(r, held[i].serial);
                dbus_message_set_serial(r, ++g_bcast_serial);
                dbus_message_set_sender(r, SDBUS_DRIVER_NAME);
                if (caller->unique) dbus_message_set_destination(r, caller->unique);
                dbus_uint32_t code = DBUS_START_REPLY_SUCCESS;
                dbus_message_append_args(r, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID);
                char *b = NULL; int len = 0;
                if (dbus_message_marshal(r, &b, &len)) { sdbus_conn_enqueue(caller, (unsigned char *)b, len, NULL, 0); dbus_free(b); ep_update(caller); }
                dbus_message_unref(r);
            }
            for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
            free(held[i].bytes); free(held[i].fds);
            continue;
        }
        /* IMPLICIT: re-route the captured (already sender-stamped) message. */
        sdbus_wire_msg w2;
        if (sdbus_wire_parse(held[i].bytes, held[i].len, &w2) == held[i].len && caller) {
            int targets[MAX_TARGETS], synth2 = 0, denied2 = 0;
            int nt = sdbus_route_targets(&w2, caller, g_names, g_conns, g_nconns,
                                         g_policy, g_replies, &synth2, &denied2, targets, MAX_TARGETS);
            for (int k = 0; k < nt; k++) {
                sdbus_conn *dst = conn_by_id(targets[k]);
                if (!dst) continue;
                if (k == 0 && held[i].nfds > 0) sdbus_conn_enqueue(dst, held[i].bytes, held[i].len, held[i].fds, held[i].nfds);
                else                            sdbus_conn_enqueue(dst, held[i].bytes, held[i].len, NULL, 0);
                ep_update(dst);
            }
            if (!(held[i].nfds > 0 && nt > 0)) for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        } else {
            for (int j = 0; j < held[i].nfds; j++) close(held[i].fds[j]);
        }
        free(held[i].bytes); free(held[i].fds);
    }
    free(held);
}
```

Note for executor: `sdbus_conn_enqueue` takes ownership semantics for fds identical to the send path in `handle_message` (fds closed after the outbound chunk is sent). Passing `held[i].fds` on the `k==0` branch transfers them; the guard closes them only when they were not transferred.

- [ ] **Step 2: Call it on name-acquire** — inside `broadcast_transitions`, in the `if (t[i].new_owner >= 0)` block (`schema-dbus.c:145`), after the NameAcquired signal is enqueued:

```c
        if (t[i].new_owner >= 0) {
            sdbus_conn *o = conn_by_id(t[i].new_owner);
            if (o) {
                DBusMessage *s = dbus_message_new_signal(SDBUS_DRIVER_PATH, SDBUS_DRIVER_NAME, "NameAcquired");
                dbus_message_append_args(s, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
                enqueue_signal(o, s); dbus_message_unref(s);
            }
            release_activation(name);          /* deliver anything held for this name */
        }
```

- [ ] **Step 3: Verify it builds**

Run: `make schema-dbus`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): deliver held messages when activated name is acquired"
```

---

### Task 8: Spawn-timeout integration

**Files:**
- Modify: `schema-dbus.c` (`main`: fold activation deadline into `epoll_wait`, add reap block)

**Interfaces:**
- Consumes: `sdbus_acts_next_deadline`, `sdbus_acts_reap_expired` (Task 2), `fail_held` (Task 6).
- Produces: nothing new.

- [ ] **Step 1: Fold the activation deadline into the wait timeout** — replace the deadline computation at `schema-dbus.c:426-428`:

```c
        int wait_ms = -1;
        long nd = sdbus_replies_next_deadline(g_replies);
        if (nd >= 0) { long now = sdbus__now_ms(); wait_ms = nd <= now ? 0 : (int)(nd - now); }
```

with:

```c
        int wait_ms = -1;
        long nd = sdbus_replies_next_deadline(g_replies);
        long ad = sdbus_acts_next_deadline(g_acts);
        if (ad >= 0 && (nd < 0 || ad < nd)) nd = ad;
        if (nd >= 0) { long now = sdbus__now_ms(); wait_ms = nd <= now ? 0 : (int)(nd - now); }
```

- [ ] **Step 2: Reap expired activations** — after the pending-reply reap block (`schema-dbus.c:450-460`), before the oq_over sweep:

```c
        /* time out activations whose service never claimed its name */
        for (;;) {
            long now = sdbus__now_ms();
            sdbus_held_msg *held = NULL; int nh = 0;
            if (!sdbus_acts_reap_expired(g_acts, now, &held, &nh)) break;
            fail_held(held, nh, DBUS_ERROR_TIMED_OUT,
                      "Activated service failed to acquire its name in time");
        }
```

- [ ] **Step 3: Verify it builds**

Run: `make schema-dbus`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): spawn-timeout reaping -> TimedOut to held callers"
```

---

### Task 9: Explicit StartServiceByName + live smoke test + full suite

**Files:**
- Modify: `schema-dbus.c` (`handle_message` to_driver path: intercept StartServiceByName)

**Interfaces:**
- Consumes: `activate_or_hold` pattern (Task 5), `g_svctab`, `g_acts`, `spawn_service`, `sdbus_names_owner` (existing).
- Produces: explicit-start handling; the whole feature is now live-testable.

- [ ] **Step 1: Intercept StartServiceByName before driver dispatch** — in `handle_message`, at the top of the `if (to_driver)` block (`schema-dbus.c:219`), before the `sdbus_codec_take` demarshal, add:

```c
    if (to_driver && w->member && !strcmp(w->member, "StartServiceByName")) {
        /* arg0 is the target name; demarshal just to read it */
        sdbus_msg dm;
        const char *target = NULL;
        if (sdbus_codec_take(raw, rawlen, &dm) == rawlen)
            target = dm.n_args > 0 && dm.args[0].type == 's' ? dm.args[0].str : NULL;
        if (target) {
            if (sdbus_names_owner(g_names, target) >= 0) {
                /* already running -> ALREADY_RUNNING */
                DBusMessage *r = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
                dbus_message_set_reply_serial(r, w->serial);
                dbus_message_set_serial(r, ++g_bcast_serial);
                dbus_message_set_sender(r, SDBUS_DRIVER_NAME);
                if (c->unique) dbus_message_set_destination(r, c->unique);
                dbus_uint32_t code = DBUS_START_REPLY_ALREADY_RUNNING;
                dbus_message_append_args(r, DBUS_TYPE_UINT32, &code, DBUS_TYPE_INVALID);
                char *b = NULL; int len = 0;
                if (dbus_message_marshal(r, &b, &len)) { sdbus_conn_enqueue(c, (unsigned char *)b, len, NULL, 0); dbus_free(b); }
                dbus_message_unref(r);
            } else {
                const sdbus_svc_ent *e = sdbus_svctab_find(g_svctab, target);
                if (!e) {
                    synth_error_wire(c, w->serial, DBUS_ERROR_SERVICE_UNKNOWN, "no such activatable service");
                } else {
                    sdbus_pending_act *pa = sdbus_acts_find(g_acts, target);
                    if (!pa) {
                        pid_t pid = spawn_service(e, g_bus_addr);
                        if (pid < 0) synth_error_wire(c, w->serial, DBUS_ERROR_SPAWN_FAILED, "fork failed");
                        else pa = sdbus_acts_begin(g_acts, target, pid, sdbus__now_ms() + SDBUS_SPAWN_TIMEOUT_MS);
                    }
                    if (pa) {
                        sdbus_held_msg m = {0};
                        m.caller_id = c->id; m.serial = w->serial; m.expects_reply = 1;
                        m.kind = SDBUS_HELD_EXPLICIT;
                        sdbus_acts_hold(pa, &m);
                    }
                }
            }
        } else {
            synth_error_wire(c, w->serial, DBUS_ERROR_INVALID_ARGS, "StartServiceByName needs a name");
        }
        if (sdbus_codec_take(raw, rawlen, &dm) == rawlen) sdbus_msg_free(&dm);
        drain_scratch(c); ep_update(c);
        consume_msg_fds(c, nfds, 0);
        return;
    }
```

Note for executor: verify `sdbus_msg` field names against `sdbus_codec.h` (`n_args`, `args[].type`, `args[].str`) and adjust if they differ. Free `dm` exactly once — restructure the double-`take` above into a single demarshal held in a local if the codec does not tolerate a second `take` on the same buffer.

- [ ] **Step 2: Build + full unit suite**

Run: `make schema-dbus && make test`
Expected: broker builds clean; `make test` reports every suite passing, including `test_sdbus_activate`.

- [ ] **Step 3: Live smoke test (scratch bus, not the live system bus)**

```bash
# start the new broker on a private socket
SCHEMA_DBUS_SOCKET=/tmp/smoke_bus ./schema-dbus --system &
BROKER=$!
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/smoke_bus
# pick a real (b1) activatable service that is safe to spawn, e.g. fwupd or a
# kauth helper; a cold call should now spawn it and get a reply, not ServiceUnknown:
busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus StartServiceByName su org.freedesktop.fwupd 0
# expect: u 1   (DBUS_START_REPLY_SUCCESS)  or  u 2 (already running)
kill $BROKER
```

Expected: `u 1` or `u 2` (not "unknown method", not ServiceUnknown). If the chosen service is not installed, pick another from `scripts/sdbus-activatable-audit.sh` output.

- [ ] **Step 4: Commit**

```bash
git add schema-dbus.c
git commit -m "feat(sdbus): explicit StartServiceByName activation + live smoke"
```

---

### Task 10: vmtest gate + audit re-verify

**Files:** none (verification only). Deploy/reboot is Jonathan's decision, NOT part of this task.

- [ ] **Step 1: Boot-test as PID 1 under QEMU**

Invoke the `schema-vmtest` skill (boots schema-init with the new broker binary in the rail). Expected: bus comes up, desktop-equivalent smoke passes, no fallback to stock dbus-daemon.

- [ ] **Step 2: Re-run the activatable audit**

Run: `sh scripts/sdbus-activatable-audit.sh`
Expected: unchanged inventory (the audit is static); note in the run that the (b1) regression set is now covered by the activation mechanism rather than by the rail.

- [ ] **Step 3: Hand off to Jonathan**

Report: unit suite green, live smoke green, vmtest green. State the deploy path (`make schema-dbus` → back up `/usr/local/bin/schema-dbus` → `sudo make install-dbus-sp1` → reboot) and the rollback binary, and STOP — do not install or reboot without his go-ahead.

---

## Self-Review

**Spec coverage:**
- Service table + parse (skip /bin/false) → Task 1. ✔
- Pending-activation table + hold/release/timeout/by-pid → Task 2. ✔
- Startup parse + globals → Task 3. ✔
- Fork+exec + User= drop + DBUS_STARTER env + CLOEXEC hygiene → Task 4. ✔
- Implicit trigger at the synth branch, hold not answer → Task 5. ✔
- SIGCHLD signalfd reaper + Spawn.ChildExited on early death → Task 6. ✔
- Release on name-acquire (implicit re-route, explicit SUCCESS) → Task 7. ✔
- Spawn timeout folded into epoll_wait + TimedOut → Task 8. ✔
- Explicit StartServiceByName + ALREADY_RUNNING race → Task 9. ✔
- vmtest gate + no unauthorized reboot → Task 10. ✔
- Out-of-scope items (session bus, SIGHUP reload, PID1 delegation, b2 set) — correctly excluded, no tasks. ✔

**Placeholder scan:** two explicit "Note for executor" items (Task 4 g_bus_addr file-scope; Task 9 codec field-name/double-take verification) are deliberate verification callouts against real code the plan cannot see the internals of, each with the concrete fix to apply — not open-ended TODOs. No "TBD"/"add error handling"/"write tests for the above".

**Type consistency:** `sdbus_svc_ent`/`sdbus_svctab`/`sdbus_acts`/`sdbus_pending_act`/`sdbus_held_msg`, `sdbus_held_kind` enum, and function names (`sdbus_svctab_parse_dir`/`_find`/`_free`, `sdbus_acts_new`/`_find`/`_begin`/`_hold`/`_take`/`_by_pid`/`_next_deadline`/`_reap_expired`/`_free`, `spawn_service`, `activate_or_hold`, `release_activation`, `fail_held`) are consistent across Tasks 1-9. `SDBUS_SPAWN_TIMEOUT_MS`, `SDBUS_SVC_DIR`, `g_svctab`, `g_acts`, `g_bus_addr` defined in Task 3-4, used in 5-9.
