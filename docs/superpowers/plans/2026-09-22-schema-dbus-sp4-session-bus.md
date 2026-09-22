# schema-dbus SP4 Session Bus Reclamation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing `schema-dbus` C broker (already owning the system bus since 2026-09-03) safe and correct to run as the session bus too, wrap it in a launcher, and cut blakbox's live desktop session over to it.

**Architecture:** Four small, independently-testable fixes to the existing broker source, all gated on a single mode signal (the `--system` CLI flag, already parsed but currently inert) so system-bus behavior is provably unchanged. A new shell launcher replaces `plasma-dbus-run-session-if-needed` at its real call site. Cutover is a logout/relogin, not a reboot.

**Tech Stack:** C (the broker, header-only modules `sdbus_*.h`), POSIX `sh` (launcher + test scripts), existing `make test` harness (plain `gcc`+`assert()` binaries, no test framework).

**Spec:** `docs/superpowers/specs/2026-09-22-schema-dbus-sp4-session-bus-design.md` (committed `2dc2c52`, plus in-plan corrections below) — this plan implements it task-by-task; read both.

## Global Constraints

- System-bus behavior must be byte-for-byte unchanged. Every fix is gated on `g_system_bus`; the system-bus code path through each modified function must be provably identical to what it does today.
- No new test framework — match the existing pattern exactly: one `.c` file per concern, `assert()`-based, compiled and run as a standalone binary via `make test`, `printf("test_X OK\n")` on success.
- No placeholders, no partial User=/env handling — every fix from the spec's four "Required code fixes" ships complete or not at all.
- Shell scripts are POSIX `sh` (`#!/bin/sh`, `set -u` or `set -e` as appropriate), matching `scripts/schema-dbus-run.sh` and `tests/sdbus_shim_check.sh` style.
- Do not touch `/usr/share/dbus-1/session.conf`, `system.conf`, or any dissolved-policy machinery — Decision 2 (no policy engine for the session bus) stands.

---

## File Structure

- **Modify** `sdbus_policy.h` — add `SDBUS_NO_POLICY_FILE_DEFAULT` (Fix 3).
- **Modify** `sdbus_activate.h` — add `sdbus_activate_should_drop_privs()`, `sdbus_activate_build_env()`/`sdbus_activate_free_env()` (Fix 1a/2 pure logic); modify `sdbus__svc_add()` to take a `default_user` param (Fix 1b); add `sdbus_svctab_parse_dirs_masked()` (Fix 4).
- **Modify** `schema-dbus.c` — add `g_system_bus` global; wire it and the new `sdbus_activate.h` functions into `spawn_service()` and `main()`.
- **Modify** `tests/test_sdbus_activate.c` — new test functions for all of the above (no changes to existing tests).
- **Modify** `tests/test_sdbus_route.c` — new test function proving `SDBUS_NO_POLICY_FILE_DEFAULT` routes broadcasts.
- **Create** `scripts/schema-dbus-session-run.sh` — the session launcher.
- **Create** `tests/sdbus_session_shim_check.sh` — automated scratch-bus proof (mirrors `tests/sdbus_shim_check.sh`'s shape, no root/`unshare` needed since session mode has no privilege boundary to cross).
- **Modify** `distros/fedora-kde/scripts/schema-plasma-autologin.sh` — swap the `:104` call site (live cutover task, blakbox only).

No file is touched by more than one task below except `schema-dbus.c` (Tasks 3 and 5, sequential) and `tests/test_sdbus_activate.c` (Tasks 2 and 4, sequential, additive).

---

### Task 1: Fix 3 — default fallback policy allows broadcast signals

**Files:**
- Modify: `sdbus_policy.h` (add define near the top, after the includes)
- Modify: `schema-dbus.c:617` (use the define instead of the inline literal)
- Test: `tests/test_sdbus_route.c` (add a new test function, called from `main()`)

**Interfaces:**
- Produces: `SDBUS_NO_POLICY_FILE_DEFAULT` (a `#define`d string literal), consumed by `schema-dbus.c`'s `main()` and by the new test.

- [x] **Step 1: Write the failing test**

Add this function to `tests/test_sdbus_route.c`, and add a call to it from `main()` (the file's `main()` currently ends with `dbus_message_unref(...)` cleanup lines then `return 0;` — add the call and its cleanup right before that final `return 0;`):

```c
static void test_no_policy_file_default_allows_broadcast(void) {
    sdbus_names *names = sdbus_names_new();
    sdbus_replies *replies = sdbus_replies_new();

    sdbus_conn s1 = {0}, s2 = {0};
    s1.id = 1; s1.uid = 1000; s1.gids[0] = 1000; s1.n_gids = 1;
    s2.id = 2; s2.uid = 1000; s2.gids[0] = 1000; s2.n_gids = 1;
    s2.matches = sdbus_match_new();
    sdbus_match_add(s2.matches, "type='signal',interface='org.sig'");
    sdbus_conn *all[] = { &s1, &s2 };

    sdbus_policy *pol = sdbus_policy_parse(SDBUS_NO_POLICY_FILE_DEFAULT);

    DBusMessage *sig = dbus_message_new_signal("/p", "org.sig", "Changed");
    dbus_message_set_serial(sig, 1);
    char *raw = NULL; int len = 0;
    assert(dbus_message_marshal(sig, &raw, &len));
    sdbus_wire_msg wm;
    assert(sdbus_wire_parse((unsigned char *)raw, len, &wm) == len);

    int synth, denied, tg[8];
    int n = sdbus_route_targets(&wm, &s1, names, all, 2, pol, replies, &synth, &denied, tg, 8);
    assert(n == 1 && tg[0] == 2 && !denied);

    dbus_free(raw);
    dbus_message_unref(sig);
    sdbus_policy_free(pol);
    sdbus_names_free(names);
    sdbus_replies_free(replies);
    sdbus_match_free(s2.matches);
    printf("test_no_policy_file_default_allows_broadcast OK\n");
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -g $(pkg-config --cflags dbus-1) tests/test_sdbus_route.c -o /tmp/schema-test-sdbus-route $(pkg-config --libs dbus-1) && /tmp/schema-test-sdbus-route`

Expected: compile FAILS — `error: 'SDBUS_NO_POLICY_FILE_DEFAULT' undeclared` (the define doesn't exist yet).

- [x] **Step 3: Add the define and switch the real call site to use it**

In `sdbus_policy.h`, right after the existing includes (after line 13, `#include <grp.h>`, before the `typedef struct` at line 15), add:

```c
/* Default policy when no SCHEMA_DBUS_POLICY file is supplied. Matches the
   session bus's actual policy (/usr/share/dbus-1/session.conf: allow
   everything) -- the session launcher deliberately never supplies a
   policy file (see SP4 design doc, Decision 2). send_type:signal is
   required for undirected/broadcast signals specifically: the
   send_destination rule's matcher (below) returns no-match when
   n_dest_names==0, so send_destination:* alone silently drops every
   broadcast (property-change notifications, portal state signals, etc)
   -- verified against this file's own test suite, which already needed
   this same line for tests/test_sdbus_route.c's broadcast case. own:*
   is included for completeness/documented-intent even though nothing
   currently gates RequestName on policy (see the SP4 design doc). */
#define SDBUS_NO_POLICY_FILE_DEFAULT \
    "context = default\n" \
    "allow = send_destination:*\n" \
    "allow = send_type:signal\n" \
    "allow = own:*\n"
```

In `schema-dbus.c`, change line 617 from:
```c
    g_policy = sdbus_policy_parse(poltext ? poltext : "context = default\nallow = send_destination:*\n");
```
to:
```c
    g_policy = sdbus_policy_parse(poltext ? poltext : SDBUS_NO_POLICY_FILE_DEFAULT);
```

- [x] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -g $(pkg-config --cflags dbus-1) tests/test_sdbus_route.c -o /tmp/schema-test-sdbus-route $(pkg-config --libs dbus-1) && /tmp/schema-test-sdbus-route`

Expected: PASS, prints `test_no_policy_file_default_allows_broadcast OK` among the other route tests, exits 0.

- [x] **Step 5: Full build + test suite sanity check**

Run: `cd /home/ajax80/projects/schema-init && make schema-dbus && make test 2>&1 | tail -30`

Expected: `schema-dbus` still builds clean; the full `make test` run (all existing suites, unrelated to this change) stays green — this confirms the define didn't break anything the old inline literal was relied on elsewhere (grep confirms `schema-dbus.c:617` was the only use site).

- [x] **Step 6: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add sdbus_policy.h schema-dbus.c tests/test_sdbus_route.c
git commit -m "$(cat <<'EOF'
fix(dbus): no-policy-file default was silently dropping broadcast signals

send_destination:* alone doesn't match undirected signals (the rule
matcher returns no-match when n_dest_names==0), so the fallback used
whenever no SCHEMA_DBUS_POLICY file is given denied every broadcast --
confirmed via the repo's own test_sdbus_route.c, which already needed
send_type:signal for its own broadcast test case. This fallback is
about to become load-bearing for the session bus (which deliberately
never supplies a policy file), where broadcast signals are common
(property-change notifications, portal state, etc).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Fix 1a/2 — pure privilege-drop and env-build logic in `sdbus_activate.h`

**Files:**
- Modify: `sdbus_activate.h` (add two new functions after `sdbus_svctab_free`, i.e. after line 127, before the `sdbus_held_msg` section at line 129)
- Test: `tests/test_sdbus_activate.c` (new test functions)

**Interfaces:**
- Produces: `int sdbus_activate_should_drop_privs(int system_bus, uid_t target_uid)`; `char **sdbus_activate_build_env(int system_bus, const char *bus_addr, char **inherited)`; `void sdbus_activate_free_env(char **env)`.
- Consumed by: Task 3 (`schema-dbus.c`'s `spawn_service`).

- [x] **Step 1: Write the failing tests**

Add to `tests/test_sdbus_activate.c` (needs `#include <sys/types.h>` added to its include block at the top, for `uid_t`):

```c
static void test_should_drop_privs(void) {
    assert(sdbus_activate_should_drop_privs(1, 1000) == 1);   /* system bus, non-root target: drop */
    assert(sdbus_activate_should_drop_privs(1, 0) == 0);      /* system bus, root target: no-op today, unchanged */
    assert(sdbus_activate_should_drop_privs(0, 1000) == 0);   /* session bus: never drop */
    assert(sdbus_activate_should_drop_privs(0, 0) == 0);      /* session bus: never drop, even if User=root */
    printf("test_should_drop_privs OK\n");
}

static int env_has(char **env, const char *entry) {
    for (char **p = env; p && *p; p++) if (!strcmp(*p, entry)) return 1;
    return 0;
}

static void test_build_env(void) {
    /* system bus: exact 3-entry clean env, unchanged from today's literal array */
    char **sys_env = sdbus_activate_build_env(1, "unix:path=/run/dbus/system_bus_socket", NULL);
    int n = 0; for (char **p = sys_env; p && *p; p++) n++;
    assert(n == 3);
    assert(env_has(sys_env, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin"));
    assert(env_has(sys_env, "DBUS_STARTER_ADDRESS=unix:path=/run/dbus/system_bus_socket"));
    assert(env_has(sys_env, "DBUS_STARTER_BUS_TYPE=system"));
    sdbus_activate_free_env(sys_env);

    /* session bus: DBUS_STARTER_BUS_TYPE=session, plus every inherited var passed through */
    char *fake_inherited[] = {
        (char *)"WAYLAND_DISPLAY=wayland-0",
        (char *)"XDG_RUNTIME_DIR=/run/user/1000",
        (char *)"HOME=/home/ajax80",
        NULL
    };
    char **sess_env = sdbus_activate_build_env(0, "unix:path=/run/user/1000/bus", fake_inherited);
    n = 0; for (char **p = sess_env; p && *p; p++) n++;
    assert(n == 5);   /* DBUS_STARTER_ADDRESS + DBUS_STARTER_BUS_TYPE + 3 inherited */
    assert(env_has(sess_env, "DBUS_STARTER_ADDRESS=unix:path=/run/user/1000/bus"));
    assert(env_has(sess_env, "DBUS_STARTER_BUS_TYPE=session"));
    assert(env_has(sess_env, "WAYLAND_DISPLAY=wayland-0"));
    assert(env_has(sess_env, "XDG_RUNTIME_DIR=/run/user/1000"));
    assert(env_has(sess_env, "HOME=/home/ajax80"));
    sdbus_activate_free_env(sess_env);

    /* session bus with no inherited vars at all -- degrades to just the 2 DBUS_STARTER_* */
    char **empty_env = sdbus_activate_build_env(0, "unix:path=/x", NULL);
    n = 0; for (char **p = empty_env; p && *p; p++) n++;
    assert(n == 2);
    sdbus_activate_free_env(empty_env);

    printf("test_build_env OK\n");
}
```

Add both calls (`test_should_drop_privs();` and `test_build_env();`) into `main()` in that file, before `printf("all sdbus_activate tests passed\n");`.

- [x] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -g -I. tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate`

Expected: compile FAILS — `implicit declaration of function 'sdbus_activate_should_drop_privs'` (and `_build_env`/`_free_env`).

- [x] **Step 3: Implement the two functions**

In `sdbus_activate.h`, right after `sdbus_svctab_free`'s closing brace (after line 127), add:

```c
/* Fix 1a (SP4 design doc): whether spawn_service should attempt to drop
   privileges before exec. System bus: drop whenever the resolved target
   isn't already root, matching stock dbus-daemon's system-activation
   behavior (unchanged from before this function existed). Session bus:
   NEVER drop -- the broker is already running, unprivileged, as the
   only user in play. Attempting it there calls initgroups()/setgid()/
   setuid() without CAP_SETGID and _exit(127)s the child before exec,
   even when the target uid matches the broker's own uid exactly. */
static inline int sdbus_activate_should_drop_privs(int system_bus, uid_t target_uid) {
    return system_bus && target_uid != 0;
}

/* Fix 2 (SP4 design doc): build the env array for an activated child.
   System bus: the pre-existing minimal clean env (PATH + DBUS_STARTER_*
   only) -- byte-identical to what spawn_service built inline before this
   function existed. Session bus: pass through the broker's own
   `inherited` environment (Wayland/XDG/HOME/etc -- session-activated
   apps like the KDE portals need these to function at all) with
   DBUS_STARTER_ADDRESS/DBUS_STARTER_BUS_TYPE=session PREPENDED so they
   are found first by a front-to-back getenv() scan even in the
   (unlikely) case `inherited` already carries stale DBUS_STARTER_*
   entries. Returns a malloc'd NULL-terminated array of malloc'd
   strings -- free with sdbus_activate_free_env(). `inherited` may be
   NULL (treated as empty) and is never modified. */
static inline char **sdbus_activate_build_env(int system_bus, const char *bus_addr,
                                              char **inherited) {
    char starter[320];
    snprintf(starter, sizeof starter, "DBUS_STARTER_ADDRESS=%s", bus_addr);

    if (system_bus) {
        char **env = malloc(4 * sizeof *env);
        env[0] = strdup("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin");
        env[1] = strdup(starter);
        env[2] = strdup("DBUS_STARTER_BUS_TYPE=system");
        env[3] = NULL;
        return env;
    }

    int n = 0;
    for (char **p = inherited; p && *p; p++) n++;
    char **env = malloc((n + 3) * sizeof *env);
    int i = 0;
    env[i++] = strdup(starter);
    env[i++] = strdup("DBUS_STARTER_BUS_TYPE=session");
    for (char **p = inherited; p && *p; p++) env[i++] = strdup(*p);
    env[i] = NULL;
    return env;
}

static inline void sdbus_activate_free_env(char **env) {
    if (!env) return;
    for (char **p = env; *p; p++) free(*p);
    free(env);
}
```

- [x] **Step 4: Run test to verify it passes**

Run: `gcc -Wall -Wextra -g -I. tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate`

Expected: PASS, all lines including `test_should_drop_privs OK`, `test_build_env OK`, `all sdbus_activate tests passed`, exit 0.

- [x] **Step 5: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add sdbus_activate.h tests/test_sdbus_activate.c
git commit -m "$(cat <<'EOF'
feat(dbus): add session-mode-aware privilege-drop and env-build logic

Pure, unit-testable helpers for Fix 1a and Fix 2 of the SP4 session-bus
design: sdbus_activate_should_drop_privs() (system bus only -- session
bus has no privilege boundary to cross, and unconditionally attempting
initgroups()/setgid()/setuid() there _exit(127)s every activation) and
sdbus_activate_build_env() (system bus keeps today's minimal clean env
byte-for-byte; session bus passes through the broker's inherited
environment, which session-activated apps like the KDE portals need
for Wayland/XDG access). Not yet wired into spawn_service() -- next task.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: wire Fix 1a/2 into `schema-dbus.c`'s `spawn_service` and `main`

**Files:**
- Modify: `schema-dbus.c` (globals block, `spawn_service`, `main`'s flag-parse section)

**Interfaces:**
- Consumes: `sdbus_activate_should_drop_privs()`, `sdbus_activate_build_env()`, `sdbus_activate_free_env()` from Task 2.
- Produces: `static int g_system_bus;` global, consumed by Task 5.

This task has no standalone unit test (`schema-dbus.c`/`main()` is the real binary, not compiled into a `make test` target) — verification is a full build plus the manual smoke check in Step 4.

- [x] **Step 1: Add the `g_system_bus` global**

In `schema-dbus.c`, right after the existing `static char g_bus_addr[256];` (line 48), add:

```c
static int g_system_bus;   /* Decision 1, SP4 design doc: --system present vs
                              absent is the single mode signal gating session-
                              bus-specific behavior throughout this file. */
```

- [x] **Step 2: Set it from the existing `--system` flag parse**

In `main()`, change:
```c
    int system_bus = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--system")) system_bus = 1;
```
to:
```c
    int system_bus = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--system")) system_bus = 1;
    g_system_bus = system_bus;
```
(the local `system_bus` var stays — it's still used later for the startup log line at line 650 — this just also populates the global other functions read.)

- [x] **Step 3: Wire `spawn_service` to use the two new functions**

Replace the whole body of `spawn_service` (lines 564-588) with:

```c
static pid_t spawn_service(const sdbus_svc_ent *e, const char *bus_addr) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) return pid;

    /* --- child --- */
    setsid();
    struct passwd *pw = getpwnam(e->user);
    if (!pw) _exit(127);                 /* unknown User= -> fail closed, never run as root */
    if (sdbus_activate_should_drop_privs(g_system_bus, pw->pw_uid)) {
        if (initgroups(e->user, pw->pw_gid) != 0) _exit(127);
        if (setgid(pw->pw_gid) != 0) _exit(127);
        if (setuid(pw->pw_uid) != 0) _exit(127);
    }
    extern char **environ;
    char **env = sdbus_activate_build_env(g_system_bus, bus_addr, environ);
    execve(e->argv[0], e->argv, env);
    _exit(127);                         /* exec failed */
}
```

(No `sdbus_activate_free_env(env)` before `execve`/`_exit` — the child either replaces its image or exits immediately either way, matching this function's existing style of not being fussy about child-process cleanup.)

- [x] **Step 4: Build and smoke-check byte-identical system-bus behavior**

Run: `cd /home/ajax80/projects/schema-init && make schema-dbus 2>&1 | tail -20`

Expected: clean build, no warnings from this file.

Then a real smoke test proving the system-bus path is unchanged — run the broker standalone on a scratch socket with `--system` and confirm it still starts and logs identically to before:

Run:
```bash
SCHEMA_DBUS_SOCKET=/tmp/sp4-smoke.sock timeout 2 ./schema-dbus --system 2>&1 | head -5
```

Expected output (first two lines): `schema-dbus: N activatable services` then `schema-dbus: listening on /tmp/sp4-smoke.sock (system=1, libdbus ...)` — same shape as before this task; `rm -f /tmp/sp4-smoke.sock` after.

- [x] **Step 5: Run the full existing test suite**

Run: `make test 2>&1 | tail -40`

Expected: all suites still green, including `test_sdbus_activate` and `test_sdbus_route` from Tasks 1-2.

- [x] **Step 6: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add schema-dbus.c
git commit -m "$(cat <<'EOF'
fix(dbus): spawn_service no longer crashes activation under an
unprivileged (session-mode) broker

Wires the SP4 design doc's Fix 1a/2 pure helpers (added last commit)
into the real spawn path: privilege-drop is now skipped when the
broker itself is unprivileged (g_system_bus=0), and the child env now
passes through the broker's own environment in that mode instead of a
system-bus-only 3-entry clean array. --system (unchanged) still yields
the exact byte-for-byte behavior spawn_service had before this change.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Fix 1b/4 — default-user threading and multi-directory servicedir search

**Files:**
- Modify: `sdbus_activate.h` (`sdbus__svc_add` signature; new `sdbus_svctab_parse_dirs_masked`)
- Test: `tests/test_sdbus_activate.c`

**Interfaces:**
- Produces: `sdbus_svctab *sdbus_svctab_parse_dirs_masked(const char **dirs, int ndirs, const char *maskfile, const char *default_user)`, consumed by Task 5.
- `sdbus_svctab_parse_dir_masked(dir, maskfile)` and `sdbus_svctab_parse_dir(dir)` keep their exact existing 1-2-arg signatures and behavior (always default to `"root"`) — no existing call site or test changes.

- [x] **Step 1: Write the failing tests**

Add to `tests/test_sdbus_activate.c`:

```c
static void test_default_user_session_mode(void) {
    char dir[] = "/tmp/sdbus-defuser-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "nouser.service",
        "[D-BUS Service]\nName=com.example.NoUser\nExec=/usr/libexec/nu\n");
    put(dir, "hasuser.service",
        "[D-BUS Service]\nName=com.example.HasUser\nExec=/usr/libexec/hu\nUser=nobody\n");

    const char *dirs[] = { dir };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 1, NULL, "ajax80");
    assert(t);

    const sdbus_svc_ent *n = sdbus_svctab_find(t, "com.example.NoUser");
    assert(n && !strcmp(n->user, "ajax80"));           /* absent User= -> the passed default */

    const sdbus_svc_ent *h = sdbus_svctab_find(t, "com.example.HasUser");
    assert(h && !strcmp(h->user, "nobody"));           /* explicit User= always wins */

    sdbus_svctab_free(t);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_default_user_session_mode OK\n");
}

static void test_multidir_override_precedence(void) {
    char dir1[] = "/tmp/sdbus-multidir1-XXXXXX";
    char dir2[] = "/tmp/sdbus-multidir2-XXXXXX";
    assert(mkdtemp(dir1));
    assert(mkdtemp(dir2));

    /* same Name in both dirs, different Exec -- dir1 (listed first) must win */
    put(dir1, "shared.service",
        "[D-BUS Service]\nName=com.example.Shared\nExec=/from/dir1\n");
    put(dir2, "shared.service",
        "[D-BUS Service]\nName=com.example.Shared\nExec=/from/dir2\n");
    /* dir2-only entry must still show up */
    put(dir2, "onlydir2.service",
        "[D-BUS Service]\nName=com.example.OnlyDir2\nExec=/from/dir2/only\n");

    const char *dirs[] = { dir1, dir2 };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    assert(t->n == 2);

    const sdbus_svc_ent *s = sdbus_svctab_find(t, "com.example.Shared");
    assert(s && !strcmp(s->argv[0], "/from/dir1"));    /* dir1 shadowed dir2 */

    const sdbus_svc_ent *o = sdbus_svctab_find(t, "com.example.OnlyDir2");
    assert(o && !strcmp(o->argv[0], "/from/dir2/only"));

    sdbus_svctab_free(t);
    char cmd1[600]; snprintf(cmd1, sizeof cmd1, "rm -rf %s", dir1); (void)system(cmd1);
    char cmd2[600]; snprintf(cmd2, sizeof cmd2, "rm -rf %s", dir2); (void)system(cmd2);
    printf("test_multidir_override_precedence OK\n");
}

static void test_multidir_missing_dir_skipped(void) {
    char dir[] = "/tmp/sdbus-multidir-missing-XXXXXX";
    assert(mkdtemp(dir));
    put(dir, "real.service", "[D-BUS Service]\nName=com.example.Real\nExec=/x\n");

    const char *dirs[] = { "/nonexistent/does/not/exist", dir };
    sdbus_svctab *t = sdbus_svctab_parse_dirs_masked(dirs, 2, NULL, "root");
    assert(t->n == 1);
    assert(sdbus_svctab_find(t, "com.example.Real"));

    sdbus_svctab_free(t);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)system(cmd);
    printf("test_multidir_missing_dir_skipped OK\n");
}
```

Add all three calls into `main()` (after the Task 2 calls), before `printf("all sdbus_activate tests passed\n");`.

- [x] **Step 2: Run test to verify it fails**

Run: `gcc -Wall -Wextra -g -I. tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate`

Expected: compile FAILS — `implicit declaration of function 'sdbus_svctab_parse_dirs_masked'`.

- [x] **Step 3: Thread `default_user` through `sdbus__svc_add` and its one call site**

In `sdbus_activate.h`, change `sdbus__svc_add`'s definition (lines 26-33) from:
```c
static inline void sdbus__svc_add(sdbus_svctab *t, const char *name,
                                  const char *exec, const char *user) {
    t->v = realloc(t->v, (t->n + 1) * sizeof *t->v);
    sdbus_svc_ent *e = &t->v[t->n++];
    e->name = strdup(name);
    e->argv = sdbus__split_argv(exec);
    e->user = strdup(user && *user ? user : "root");
}
```
to:
```c
static inline void sdbus__svc_add(sdbus_svctab *t, const char *name,
                                  const char *exec, const char *user,
                                  const char *default_user) {
    t->v = realloc(t->v, (t->n + 1) * sizeof *t->v);
    sdbus_svc_ent *e = &t->v[t->n++];
    e->name = strdup(name);
    e->argv = sdbus__split_argv(exec);
    e->user = strdup(user && *user ? user : default_user);
}
```

Then update its ONE existing call site, inside `sdbus_svctab_parse_dir_masked` (currently line 101):
```c
        sdbus__svc_add(t, name, exec, user);
```
to:
```c
        sdbus__svc_add(t, name, exec, user, "root");   /* unchanged behavior for this back-compat path */
```

This keeps `sdbus_svctab_parse_dir_masked`/`sdbus_svctab_parse_dir` (and every existing test that calls them) byte-for-byte unchanged.

- [x] **Step 4: Add `sdbus_svctab_parse_dirs_masked`**

In `sdbus_activate.h`, right after `sdbus_svctab_free`'s closing brace (i.e. immediately before the two Task 2 functions you added, or immediately after them — either order is fine, they're independent; place it after `sdbus_svctab_free` and before the Task 2 functions to keep all svctab-table functions grouped together) — note it must come **after** `sdbus_svctab_find` (line ~112-117) since it calls that function:

```c
/* Fix 4 (SP4 design doc): like sdbus_svctab_parse_dir_masked, but scans
   `dirs` in order and applies override precedence -- the first dir to
   define a given Name= wins, later dirs are skipped for that name (a
   missing/unreadable dir is silently skipped, same as the single-dir
   version already does via opendir()'s NULL-on-failure check). Needed
   for the session bus, which searches at least two directories
   (~/.local/share/dbus-1/services overriding /usr/share/dbus-1/services
   -- see the SP4 design doc's Fix 4 section for why the user-level one
   specifically is load-bearing, not just generically nice-to-have).
   default_user is Fix 1b: what an entry's User= resolves to when absent
   from the .service file -- "root" for the system bus (matching
   sdbus_svctab_parse_dir_masked exactly), the invoking user for the
   session bus. */
static inline sdbus_svctab *sdbus_svctab_parse_dirs_masked(const char **dirs, int ndirs,
                                                            const char *maskfile,
                                                            const char *default_user) {
    sdbus_svctab *t = calloc(1, sizeof *t);
    sdbus_strset *mask = sdbus_masklist_load(maskfile);
    for (int di = 0; di < ndirs; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 9 || strcmp(de->d_name + l - 8, ".service")) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dirs[di], de->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[2048], name[2048] = "", exec[2048] = "", user[2048] = "";
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (!strncmp(line, "Name=", 5))      snprintf(name, sizeof name, "%s", line + 5);
                else if (!strncmp(line, "Exec=", 5)) snprintf(exec, sizeof exec, "%s", line + 5);
                else if (!strncmp(line, "User=", 5)) snprintf(user, sizeof user, "%s", line + 5);
            }
            fclose(f);
            if (!name[0] || !exec[0]) continue;
            if (!strcmp(exec, "/bin/false") || !strcmp(exec, "/usr/bin/false")) continue;
            if (sdbus_masklist_has(mask, name)) continue;
            if (sdbus_svctab_find(t, name)) continue;   /* first dir wins */
            sdbus__svc_add(t, name, exec, user, default_user);
        }
        closedir(d);
    }
    sdbus_masklist_free(mask);
    return t;
}
```

- [x] **Step 5: Run test to verify it passes**

Run: `gcc -Wall -Wextra -g -I. tests/test_sdbus_activate.c -o /tmp/schema-test-sdbus-activate && /tmp/schema-test-sdbus-activate`

Expected: PASS — all previous OKs plus `test_default_user_session_mode OK`, `test_multidir_override_precedence OK`, `test_multidir_missing_dir_skipped OK`, `all sdbus_activate tests passed`, exit 0.

- [x] **Step 6: Full test suite + build sanity check**

Run: `cd /home/ajax80/projects/schema-init && make schema-dbus && make test 2>&1 | tail -40`

Expected: clean build, all suites green.

- [x] **Step 7: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add sdbus_activate.h tests/test_sdbus_activate.c
git commit -m "$(cat <<'EOF'
feat(dbus): multi-directory servicedir search with override precedence

Fix 1b/4 of the SP4 session-bus design: sdbus__svc_add now takes an
explicit default_user (threaded through, "root" preserved exactly at
the existing single-dir call site); new sdbus_svctab_parse_dirs_masked
scans a directory list in order, first dir wins on a Name= collision.
Needed for the session bus, which must search
~/.local/share/dbus-1/services (override) then
/usr/share/dbus-1/services -- traced this to a concrete, currently-live
mechanism: org.freedesktop.systemd1.service (the schema-systemd1-session
shim's own activation entry, no User= line) only exists in the former
directory. Not yet wired into schema-dbus.c's main() -- next task.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: wire Fix 4 into `schema-dbus.c`'s `main`

**Files:**
- Modify: `schema-dbus.c` (svctab-build section of `main`, lines 621-624)

**Interfaces:**
- Consumes: `sdbus_svctab_parse_dirs_masked()` from Task 4, `g_system_bus` from Task 3.

No standalone unit test (same reason as Task 3) — verified by build + the scratch-bus proof in Task 7, which exercises this exact code path end-to-end.

- [x] **Step 1: Add the multi-dir env var and default-user computation, replace the svctab build**

In `schema-dbus.c`, replace lines 621-624:
```c
    const char *svcdir = getenv("SCHEMA_DBUS_SVCDIR");
    const char *maskfile = getenv("SCHEMA_DBUS_MASKFILE");
    g_svctab = sdbus_svctab_parse_dir_masked(svcdir ? svcdir : SDBUS_SVC_DIR,
                                             maskfile ? maskfile : SDBUS_MASK_FILE);
```
with:
```c
    const char *svcdir = getenv("SCHEMA_DBUS_SVCDIR");
    const char *svcdirs_env = getenv("SCHEMA_DBUS_SVCDIRS");
    const char *maskfile = getenv("SCHEMA_DBUS_MASKFILE");

    struct passwd *self_pw = g_system_bus ? NULL : getpwuid(getuid());
    const char *default_user = g_system_bus ? "root" : (self_pw ? self_pw->pw_name : "root");

    const char *dirs[SDBUS_MAX_SVCDIRS];
    int ndirs = 0;
    char *svcdirs_buf = NULL;
    if (svcdirs_env) {
        svcdirs_buf = strdup(svcdirs_env);
        for (char *p = strtok(svcdirs_buf, ":"); p && ndirs < SDBUS_MAX_SVCDIRS; p = strtok(NULL, ":"))
            dirs[ndirs++] = p;
    } else {
        dirs[0] = svcdir ? svcdir : SDBUS_SVC_DIR;
        ndirs = 1;
    }
    g_svctab = sdbus_svctab_parse_dirs_masked(dirs, ndirs,
                                              maskfile ? maskfile : SDBUS_MASK_FILE,
                                              default_user);
    free(svcdirs_buf);
```

And add the constant next to the other `#define`s near the top (after `#define SDBUS_MASK_FILE "/etc/schema-dbus/masked"` at line 50):
```c
#define SDBUS_MAX_SVCDIRS 8
```

Note: on the system bus, `SCHEMA_DBUS_SVCDIRS` is never set (the system-bus launcher `schema-dbus-run.sh` doesn't set it), so this always takes the `else` branch — `dirs[0] = svcdir ? svcdir : SDBUS_SVC_DIR; ndirs = 1;` — functionally identical single-dir behavior to before, just routed through the new multi-dir function instead of the old 2-arg wrapper. `default_user` is `"root"` on the system bus either way, matching today exactly.

- [x] **Step 2: Build and smoke-check**

Run: `cd /home/ajax80/projects/schema-init && make schema-dbus 2>&1 | tail -20`

Expected: clean build.

Run a real smoke test proving system-bus single-dir behavior is unchanged:
```bash
SCHEMA_DBUS_SOCKET=/tmp/sp4-smoke2.sock timeout 2 ./schema-dbus --system 2>&1 | head -2
```
Expected: `schema-dbus: N activatable services` where N matches what it printed before this task (same `/usr/share/dbus-1/system-services` dir, same count) — `rm -f /tmp/sp4-smoke2.sock` after.

Then a real smoke test proving the new session path works end to end:
```bash
mkdir -p /tmp/sp4-smoke-svcdir1 /tmp/sp4-smoke-svcdir2
cat > /tmp/sp4-smoke-svcdir1/test.service <<'EOF'
[D-BUS Service]
Name=com.example.SmokeTest
Exec=/bin/true
EOF
SCHEMA_DBUS_SOCKET=/tmp/sp4-smoke3.sock \
SCHEMA_DBUS_SVCDIRS=/tmp/sp4-smoke-svcdir1:/tmp/sp4-smoke-svcdir2 \
SCHEMA_DBUS_MASKFILE=/dev/null \
timeout 2 ./schema-dbus 2>&1 | head -2
rm -f /tmp/sp4-smoke3.sock; rm -rf /tmp/sp4-smoke-svcdir1 /tmp/sp4-smoke-svcdir2
```
Expected: `schema-dbus: 1 activatable services` (no `--system`, so this ran in session mode, found the one entry across the two dirs).

- [x] **Step 3: Run the full existing test suite**

Run: `make test 2>&1 | tail -40`

Expected: all suites green.

- [x] **Step 4: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add schema-dbus.c
git commit -m "$(cat <<'EOF'
feat(dbus): main() supports SCHEMA_DBUS_SVCDIRS for multi-dir activation search

Wires sdbus_svctab_parse_dirs_masked (added last commit) into the real
startup path. SCHEMA_DBUS_SVCDIRS (colon-separated, PATH convention)
takes precedence when set; unset falls back to the existing single-dir
SCHEMA_DBUS_SVCDIR/SDBUS_SVC_DIR behavior unchanged -- the system-bus
launcher never sets SCHEMA_DBUS_SVCDIRS, so system-bus startup takes
the identical single-entry path it always has. default_user (Fix 1b)
is computed here too: "root" for --system, the broker's own invoking
user otherwise (via getpwuid(getuid())).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `scripts/schema-dbus-session-run.sh` launcher

**Files:**
- Create: `scripts/schema-dbus-session-run.sh`

**Interfaces:**
- Consumes: the `schema-dbus` binary built by Tasks 1-5 (via the same `find_bin` lookup pattern as `scripts/schema-dbus-run.sh`).
- Produces: a script invoked as `schema-dbus-session-run.sh <command> [args...]`, execing `<command>` as a child once the session bus is up — the exact call shape Task 8's cutover needs at `schema-plasma-autologin.sh:104`.

No unit test (it's a shell launcher, not a header function) — verified by the scratch-bus proof in Task 7 (which exercises the broker directly, the piece with real logic) and a direct manual run in this task's own steps.

- [x] **Step 1: Write the script**

Create `scripts/schema-dbus-session-run.sh`:

```sh
#!/bin/sh
# schema-dbus-session-run.sh <command> [args...]
#
# Drop-in replacement for /usr/libexec/plasma-dbus-run-session-if-needed at
# its call site in schema-plasma-autologin.sh:104. Starts the session bus
# (the schema-dbus C broker, falling back to stock dbus-daemon on failure),
# exports DBUS_SESSION_BUS_ADDRESS, then execs the given command as a child
# -- same shape as the tool it replaces, so the caller's exit-code/lifetime
# tracking (schema-plasma-autologin.sh's `RC=$?` loop) is unaffected.
#
# No --system flag is passed to schema-dbus: that absence is what puts the
# broker in session mode (see schema-dbus.c's g_system_bus). No
# SCHEMA_DBUS_POLICY is set either: the broker's no-policy-file default
# (SDBUS_NO_POLICY_FILE_DEFAULT) already matches session.conf's allow-all
# stance -- see the SP4 design doc, Decision 2.
set -u

if [ $# -lt 1 ]; then
    echo "usage: schema-dbus-session-run.sh <command> [args...]" >&2
    exit 1
fi

XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"

self_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

find_bin() {
    for p in /usr/local/bin/"$1" /usr/bin/"$1" /sbin/"$1" \
             "$self_dir/../$1" "$self_dir/$1"; do
        [ -x "$p" ] && { echo "$p"; return 0; }
    done
    command -v "$1" 2>/dev/null
}

BROKER=$(find_bin schema-dbus)
STOCK=$(find_bin dbus-daemon)

wait_for_socket() {   # up to 2s; a local fork+bind is normally near-instant
    i=0
    while [ "$i" -lt 10 ]; do
        [ -S "$XDG_RUNTIME_DIR/bus" ] && return 0
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

if [ -n "$BROKER" ]; then
    SCHEMA_DBUS_SOCKET="$XDG_RUNTIME_DIR/bus" \
    SCHEMA_DBUS_SVCDIRS="$HOME/.local/share/dbus-1/services:/usr/share/dbus-1/services" \
    SCHEMA_DBUS_MASKFILE=/dev/null \
    "$BROKER" &
fi

if [ -z "$BROKER" ] || ! wait_for_socket; then
    echo "schema-dbus-session-run: broker unavailable — falling back to stock dbus-daemon" >&2
    if [ -z "$STOCK" ]; then
        echo "schema-dbus-session-run: no dbus-daemon to fall back to — no session bus" >&2
        exit 1
    fi
    "$STOCK" --session --address="unix:path=$XDG_RUNTIME_DIR/bus" --nofork &
    wait_for_socket || echo "schema-dbus-session-run: socket still not up, continuing anyway" >&2
fi

exec "$@"
```

- [x] **Step 2: Make it executable and shellcheck it**

Run:
```bash
chmod +x /home/ajax80/projects/schema-init/scripts/schema-dbus-session-run.sh
shellcheck /home/ajax80/projects/schema-init/scripts/schema-dbus-session-run.sh || true
```
Expected: no `shellcheck` errors (warnings about `$()` vs backticks etc. are fine if any appear; there shouldn't be any real ones given the script mirrors `schema-dbus-run.sh`'s already-shellcheck-clean style). If `shellcheck` isn't installed, note that and move on — it's a nice-to-have, not a blocker.

- [x] **Step 3: Manual smoke test — broker path**

Run (as the current user, not root — this is exactly how it'll run in the real session):
```bash
cd /home/ajax80/projects/schema-init
XDG_RUNTIME_DIR=/tmp/sp4-launcher-smoke ./scripts/schema-dbus-session-run.sh sh -c 'echo "child running, DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS"; busctl --address="$DBUS_SESSION_BUS_ADDRESS" list --acquired --no-legend | head -3'
```
(`XDG_RUNTIME_DIR=/tmp/sp4-launcher-smoke` avoids touching the real `/run/user/$(id -u)/bus` while testing — create `/tmp/sp4-launcher-smoke` first with `mkdir -p /tmp/sp4-launcher-smoke` if it doesn't exist.)

Expected: prints `child running, DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/sp4-launcher-smoke/bus`, then at least `org.freedesktop.DBus` in the acquired-names list — proves the broker started, the socket came up, and `exec "$@"` correctly ran the child with the right env. Clean up: `pkill -f 'schema-dbus$'` for any leftover broker from this test that outlived the smoke script (the script's own broker child is backgrounded and NOT killed when `exec "$@"`'s child exits, matching real session behavior where the broker should keep running after the launched command starts) — check with `pgrep -af schema-dbus` first to confirm which PID to kill, don't blindly kill a broker you didn't just start.

- [x] **Step 4: Manual smoke test — fallback path**

Run (rename the broker temporarily to force the fallback branch):
```bash
cd /home/ajax80/projects/schema-init
mv schema-dbus schema-dbus.smoketest-hidden
mkdir -p /tmp/sp4-launcher-smoke2
XDG_RUNTIME_DIR=/tmp/sp4-launcher-smoke2 ./scripts/schema-dbus-session-run.sh sh -c 'echo "child running under: $DBUS_SESSION_BUS_ADDRESS"'
mv schema-dbus.smoketest-hidden schema-dbus
```
Expected: stderr shows `schema-dbus-session-run: broker unavailable — falling back to stock dbus-daemon`, then the child still prints `child running under: unix:path=/tmp/sp4-launcher-smoke2/bus` — proves the fallback path (Decision 4, self-heal) actually works, not just reads correctly. Clean up any leftover `dbus-daemon --session --address=unix:path=/tmp/sp4-launcher-smoke2/bus` process the same careful way as Step 3, and `rm -rf /tmp/sp4-launcher-smoke /tmp/sp4-launcher-smoke2`.

- [x] **Step 5: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add scripts/schema-dbus-session-run.sh
git commit -m "$(cat <<'EOF'
feat(dbus): add schema-dbus-session-run.sh, the session-bus launcher

Drop-in for /usr/libexec/plasma-dbus-run-session-if-needed at its real
call site (schema-plasma-autologin.sh:104, traced while writing this
plan -- not inside plasma-session-start.sh as the design doc originally
assumed). Starts schema-dbus in session mode (no --system, SVCDIRS set
to the two session servicedirs, no policy file), execs the given
command as a child once the socket is up, self-heals to stock
dbus-daemon on any failure. Not yet wired into the live autologin
script -- covered by the scratch-bus proof next, then the real cutover.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: automated scratch-bus proof

**Files:**
- Create: `tests/sdbus_session_shim_check.sh`

**Interfaces:**
- Consumes: the `schema-dbus` binary (built by Tasks 1-5) and `schema-systemd1-session` (the existing, unmodified session shim already live on blakbox at `/usr/local/bin/schema-systemd1-session` — this script uses a local copy from the repo if one exists there, else skips that specific check; see Step 1).

This is the design doc's "Isolated scratch-bus proof" section made concrete and automated (mirroring `tests/sdbus_shim_check.sh`'s shape) instead of a manual by-hand checklist — no root/`unshare` needed, since session mode has no privilege boundary to prove (unlike the system-bus shim-check, which specifically needed to fake uid 0).

- [x] **Step 1: Write the script**

Create `tests/sdbus_session_shim_check.sh`:

```sh
#!/bin/sh
# Scratch-bus proof for the SP4 session-bus fixes (Fix 1a/1b/2/3/4),
# mirroring tests/sdbus_shim_check.sh's shape but for session mode --
# no root/unshare needed, since the session bus has no privilege boundary
# (the broker runs as whoever invokes this script, exactly like it would
# for a real login).
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
SOCK="$WORK/bus.sock"
ADDR="unix:path=$SOCK"
LOCALDIR="$WORK/local-services"
SYSDIR="$WORK/system-services"
mkdir -p "$LOCALDIR" "$SYSDIR"

BPID=""
cleanup() {
    [ -n "$BPID" ] && kill "$BPID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- fixture: a real, User=-less .service file that writes a marker file
# recording who ran it -- the direct regression test for Fix 1a/1b (must
# NOT be root, since this whole script runs unprivileged) and Fix 4 dir 1
# specifically (this file only lives in LOCALDIR, mirroring
# ~/.local/share/dbus-1/services' real role -- see the SP4 design doc's
# Fix 4 section on org.freedesktop.systemd1.service).
MARKER="$WORK/activated-as-uid"
cat > "$LOCALDIR/com.example.SmokeTest.service" <<EOF
[D-BUS Service]
Name=com.example.SmokeTest
Exec=/bin/sh -c "id -u > $MARKER; sleep 5"
EOF

# --- a same-named entry in SYSDIR that must be shadowed (Fix 4 override
# precedence) -- if this one wins instead, MARKER never gets written
# because this Exec= doesn't touch it.
cat > "$SYSDIR/com.example.SmokeTest.service" <<'EOF'
[D-BUS Service]
Name=com.example.SmokeTest
Exec=/bin/sh -c "sleep 5"
EOF

# --- start the broker in session mode: no --system, SVCDIRS set, no
# policy file (Fix 3's default applies) ------------------------------
SCHEMA_DBUS_SOCKET="$SOCK" \
SCHEMA_DBUS_SVCDIRS="$LOCALDIR:$SYSDIR" \
SCHEMA_DBUS_MASKFILE=/dev/null \
    "$ROOT/schema-dbus" >"$WORK/broker.log" 2>&1 &
BPID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FAIL: broker did not bind $SOCK"; cat "$WORK/broker.log"; exit 1; }

echo "1. RequestName / ListNames work on the session broker"
busctl --address="$ADDR" list --acquired --no-legend >/dev/null 2>"$WORK/e1" \
    || { echo "FAIL: ListNames errored"; cat "$WORK/e1" "$WORK/broker.log"; exit 1; }

echo "2. cold-activate a real, User=-less .service (Fix 1a/1b + Fix 4 dir precedence)"
busctl --address="$ADDR" call com.example.SmokeTest / org.freedesktop.DBus.Peer Ping \
    >/dev/null 2>"$WORK/e2" || true   # activation itself is what we're checking, not this call's own success
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$MARKER" ] && break; sleep 0.2; done
[ -f "$MARKER" ] || { echo "FAIL: activated service never wrote its marker (wrong dir won, or spawn failed)"; cat "$WORK/broker.log"; exit 1; }
GOT_UID="$(cat "$MARKER")"
MY_UID="$(id -u)"
[ "$GOT_UID" = "$MY_UID" ] || { echo "FAIL: activated as uid $GOT_UID, expected $MY_UID (Fix 1a/1b regression)"; exit 1; }

echo "3. broadcast signal delivered to a subscribed match (Fix 3)"
busctl --address="$ADDR" wait / com.example.sig Changed >"$WORK/wait.log" 2>&1 &
WAITPID=$!
sleep 0.3
busctl --address="$ADDR" emit / com.example.sig Changed >/dev/null 2>"$WORK/e3" \
    || { echo "FAIL: emit errored"; cat "$WORK/e3"; kill "$WAITPID" 2>/dev/null; exit 1; }
if wait "$WAITPID"; then
    :
else
    echo "FAIL: busctl wait never saw the broadcast (Fix 3 regression)"; cat "$WORK/wait.log"; exit 1
fi

echo "4. broker still alive throughout"
kill -0 "$BPID" 2>/dev/null || { echo "FAIL: broker died"; cat "$WORK/broker.log"; exit 1; }

echo "sdbus_session_shim_check: ALL OK"
```

- [x] **Step 2: Make it executable and run it**

Run:
```bash
cd /home/ajax80/projects/schema-init
chmod +x tests/sdbus_session_shim_check.sh
make schema-dbus
./tests/sdbus_session_shim_check.sh
```

Expected: `1.`...`4.` each print with no `FAIL:` lines, ending in `sdbus_session_shim_check: ALL OK`, exit 0.

If step 2's activation check fails, debug with `cat` on the script's own `$WORK/broker.log` (the script deletes `$WORK` on exit via its `cleanup` trap — temporarily comment out the `trap cleanup EXIT` line while debugging, then restore it).

- [x] **Step 3: Add it to the Makefile's `test-all` awareness (optional but recommended)**

Check whether `test-all`'s Python-glob loop (`for t in tests/test_*.py`) or anything else would need to know about this new `.sh` file. It won't (that loop only picks up `test_*.py`), and `tests/sdbus_shim_check.sh` isn't wired into `make test`/`make test-all` either (it's a standalone integration script, run manually or from a livetest harness) — confirm this by grepping:

Run: `grep -n "sdbus_shim_check" Makefile`

Expected: no output (confirms the existing sibling script also isn't Makefile-wired, so not wiring the new one matches established convention — nothing to change here).

- [x] **Step 4: Commit**

```bash
cd /home/ajax80/projects/schema-init
git add tests/sdbus_session_shim_check.sh
git commit -m "$(cat <<'EOF'
test(dbus): add automated scratch-bus proof for the SP4 session fixes

Mirrors tests/sdbus_shim_check.sh's shape (no root/unshare needed --
session mode has no privilege boundary to fake). Proves, end to end
against the real binary: RequestName/ListNames work; a real
User=-less .service cold-activates as the invoking uid, not root
(Fix 1a/1b), from the correct directory when two dirs define the same
name (Fix 4 override precedence); an undirected broadcast signal
reaches a subscribed match (Fix 3). This is the design doc's "Isolated
scratch-bus proof" step, automated instead of a manual checklist.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: live cutover on blakbox

**Files:**
- Modify: `distros/fedora-kde/scripts/schema-plasma-autologin.sh:104` (repo copy)
- Modify (deploy): `/usr/local/bin/schema-plasma-autologin.sh` (live rail copy)

**Interfaces:** none — this is the manual, Jonathan-present cutover step. No new code; everything it exercises was proven in Tasks 1-7.

**This task requires Jonathan present and available to log back in — do not run it unattended, and confirm with him immediately before Step 2 (the point of no return for the current login session).**

- [ ] **Step 1: Back up the live file**

Run:
```bash
sudo cp /usr/local/bin/schema-plasma-autologin.sh /usr/local/bin/schema-plasma-autologin.sh.bak-sp4-cutover-$(date +%Y%m%d)
```

- [ ] **Step 2: Edit the repo copy's call site**

In `distros/fedora-kde/scripts/schema-plasma-autologin.sh`, change line 104 from:
```sh
        /usr/libexec/plasma-dbus-run-session-if-needed /usr/local/bin/plasma-session-start.sh )
```
to:
```sh
        /usr/local/bin/schema-dbus-session-run.sh /usr/local/bin/plasma-session-start.sh )
```

- [ ] **Step 3: Deploy the launcher and the updated autologin script**

Run:
```bash
sudo cp /home/ajax80/projects/schema-init/scripts/schema-dbus-session-run.sh /usr/local/bin/schema-dbus-session-run.sh
sudo chmod +x /usr/local/bin/schema-dbus-session-run.sh
sudo cp /home/ajax80/projects/schema-init/distros/fedora-kde/scripts/schema-plasma-autologin.sh /usr/local/bin/schema-plasma-autologin.sh
```

Also confirm the just-built `schema-dbus` binary (with all four fixes) is the one deployed at `/usr/local/bin/schema-dbus` and `/usr/bin/schema-dbus` (wherever the launcher's `find_bin` will actually find it) — check with `which schema-dbus` and `schema-dbus --version` (or equivalent) against the repo build's own version/hash, matching whatever verification pattern was used for the original system-bus cutover ([[project_schema_dbus]]).

- [ ] **Step 4: Log out and back in (Jonathan performs this step)**

No reboot. Confirm with Jonathan before proceeding, then have him log out and back in (or restart the autologin service if that's the faster path on this box).

- [ ] **Step 5: Verify on the fresh session**

Run each of these and confirm the expected result:

```bash
busctl --user list --acquired --no-legend | grep -c .
```
Expected: a healthy-looking count of owned names (dozens), not near-zero — near-zero would mean the broker came up but almost nothing registered, a red flag.

```bash
pgrep -af xdg-desktop-portal
```
Expected: all three portal processes present (`xdg-desktop-portal`, `-kde`, `-gtk`), same as before cutover.

```bash
pgrep -af schema-dbus
```
Expected: one `schema-dbus` process (no `--system` in its argv), confirming the session broker — not stock `dbus-daemon` — is what's running.

Then, interactively: open Dolphin, "Open With → KWrite" on any file. Expected: KWrite actually launches — this cold-activates `org.freedesktop.systemd1` through `~/.local/share/dbus-1/services/org.freedesktop.systemd1.service` (Fix 1a/1b/4's real-world regression test, per the design doc), the exact path proven synthetically in Task 7.

- [ ] **Step 6: If anything in Step 5 fails — rollback**

```bash
sudo cp /usr/local/bin/schema-plasma-autologin.sh.bak-sp4-cutover-$(date +%Y%m%d) /usr/local/bin/schema-plasma-autologin.sh
```
Then log out and back in again. No reboot, no boot-guard involvement, fully recoverable — if the live session itself is unusable, do this from another host over SSH instead.

- [ ] **Step 7: Commit the repo-side change (only after Step 5 verification passes)**

```bash
cd /home/ajax80/projects/schema-init
git add distros/fedora-kde/scripts/schema-plasma-autologin.sh
git commit -m "$(cat <<'EOF'
feat(dbus): cut the session bus over to schema-dbus on blakbox

schema-plasma-autologin.sh:104 now invokes schema-dbus-session-run.sh
in place of the stock plasma-dbus-run-session-if-needed. Verified live
on blakbox 2026-09-22: portals registered, Dolphin's Open-With-KWrite
(the org.freedesktop.systemd1 cold-activation path) works, no reboot
needed. Closes SP4 -- schema-dbus now owns both the system and session
D-Bus on blakbox.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review Notes (writing-plans skill, completed before handoff)

**Spec coverage:** all four "Required code fixes" sections map 1:1 to Tasks 1-5; the Launcher section maps to Task 6; the Testing plan's "Isolated scratch-bus proof" maps to Task 7 (automated, stronger than the spec's by-hand version); the Cutover section maps to Task 8. Decision 3 (systemd1-session shim stays separate) required no task — it's a non-change, verified implicitly by Task 7/8 both depending on that shim continuing to work unmodified.

**Placeholder scan:** none found — every step has real code, real commands, real expected output.

**Type consistency:** `sdbus_svctab_parse_dirs_masked`'s signature (`const char **dirs, int ndirs, const char *maskfile, const char *default_user`) is identical everywhere it's declared (Task 4), implemented (Task 4), and called (Task 5, Task 7's fixture use is via the real binary's env vars, not a direct call). `sdbus_activate_should_drop_privs`/`sdbus_activate_build_env`/`sdbus_activate_free_env` signatures match between Task 2's tests, Task 2's implementation, and Task 3's call sites. `g_system_bus` is declared once (Task 3) and only read (never redeclared) in Tasks 3 and 5.
