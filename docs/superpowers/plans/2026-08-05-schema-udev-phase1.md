# schema-udev Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `schema-udev`, a standalone native daemon that listens on the kernel uevent netlink, matches events against `.dev` schema rules, and runs `on_add`/`on_remove` hooks — running alongside the real `systemd-udevd`.

**Architecture:** Pure logic (uevent parse, rule model, glob match, rule loading) lives in a header `schema-udev.h` so the unit tests `#include` it; the daemon wiring (netlink socket, credential check, `signalfd` poll loop, `fork`/`exec`, `SIGCHLD` reap, `SIGHUP` reload) lives in `schema-udev.c`. Binds netlink **group 1** (raw kernel events) only — never group 2 — so libudev clients (PipeWire, NetworkManager, KDE) are untouched.

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -D_GNU_SOURCE`), Linux netlink (`NETLINK_KOBJECT_UEVENT`), `signalfd(2)`, `poll(2)`, `fnmatch(3)`. Freestanding assert-based unit tests compiled in the Makefile `test:` target. Logs to stderr → journal-sink.

## Global Constraints

- Language: C99. Compile flags: `-std=c99 -Wall -Wextra -D_GNU_SOURCE`. Must build clean with `-Wall -Wextra`.
- Netlink bind: `nl_groups = 1` (kernel uevents) ONLY. Never group 2.
- Security: every datagram must pass BOTH `SCM_CREDENTIALS` `uid == 0` AND sender `nl_pid == 0`, else drop.
- `.dev` / `.svc` parse parity: `strchr(line, '=')`; strip trailing `\r\n` via `strcspn`; lines without `=` are skipped (whole-line `#` comments only — NO inline comment stripping). Keep consistent with `service_load_one` in `service.c`.
- Rule match: `fnmatch(3)` glob; ALL `match_*` conditions must match (AND); a rule with 0 conditions matches nothing; an absent uevent key fails the match.
- Hook exec: child exports EVERY uevent key as env (including `ACTION` explicitly), then `execl("/bin/sh","sh","-c",hook,NULL)`; parent never blocks.
- `SIGCHLD`: drain with `while (waitpid(-1, NULL, WNOHANG) > 0);`.
- Not `critical`, no `dep=`. Binary installs to `/usr/bin/schema-udev` (added to Makefile `BINS`).
- Info-level logging = matched rules firing only (never log every uevent — coldplug floods 400+).
- Phase 1 does NOT coldplug, manage `/dev` symlinks, write `/run/udev`, or add a hook timeout. Those are Phases 2–3.

---

### Task 1: uevent parser (pure, header + unit test)

**Files:**
- Create: `schema-udev.h`
- Test: `tests/test_uevent_parse.c`
- Modify: `Makefile` (add compile line to `test:` target)

**Interfaces:**
- Produces: `struct uevent { char key[32][64]; char val[32][512]; int n; };`
- Produces: `const char *uevent_get(const struct uevent *ev, const char *key)` → value or `NULL`.
- Produces: `int uevent_parse(const char *buf, size_t len, struct uevent *ev)` → `0` ok, `-1` malformed.

- [ ] **Step 1: Write the failing test** — `tests/test_uevent_parse.c`

```c
#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Build a raw kernel netlink buffer: "hdr\0K=V\0K=V\0..." Returns total length. */
static size_t mkbuf(char *dst, const char *hdr, const char **kv, int nkv) {
    size_t o = 0;
    memcpy(dst + o, hdr, strlen(hdr) + 1); o += strlen(hdr) + 1;
    for (int i = 0; i < nkv; i++) { memcpy(dst + o, kv[i], strlen(kv[i]) + 1); o += strlen(kv[i]) + 1; }
    return o;
}

int main(void) {
    char buf[4096];
    struct uevent ev;

    /* real captured usb_device add event */
    const char *kv1[] = {
        "ACTION=add", "DEVPATH=/devices/pci0000:00/usb1/1-4", "SUBSYSTEM=usb",
        "DEVNAME=/dev/bus/usb/001/002", "DEVTYPE=usb_device", "DRIVER=usb",
        "PRODUCT=a16f/304/620", "BUSNUM=001", "DEVNUM=002", "MAJOR=189", "MINOR=1"
    };
    size_t n1 = mkbuf(buf, "add@/devices/pci0000:00/usb1/1-4", kv1, 11);
    assert(uevent_parse(buf, n1, &ev) == 0);
    assert(ev.n == 11);
    assert(strcmp(uevent_get(&ev, "ACTION"), "add") == 0);
    assert(strcmp(uevent_get(&ev, "SUBSYSTEM"), "usb") == 0);
    assert(strcmp(uevent_get(&ev, "PRODUCT"), "a16f/304/620") == 0);
    assert(strcmp(uevent_get(&ev, "DEVNAME"), "/dev/bus/usb/001/002") == 0);
    assert(uevent_get(&ev, "NOPE") == NULL);

    /* remove event on a tty */
    const char *kv2[] = { "ACTION=remove", "SUBSYSTEM=tty", "DEVNAME=/dev/ttyUSB0" };
    size_t n2 = mkbuf(buf, "remove@/devices/x/ttyUSB0", kv2, 3);
    assert(uevent_parse(buf, n2, &ev) == 0);
    assert(strcmp(uevent_get(&ev, "ACTION"), "remove") == 0);

    /* malformed: no NUL at all -> not a kernel uevent */
    memcpy(buf, "garbage-no-nul", 14);
    assert(uevent_parse(buf, 14, &ev) == -1);

    /* malformed: header present but no ACTION key -> reject */
    const char *kv3[] = { "SUBSYSTEM=usb", "DEVPATH=/x" };
    size_t n3 = mkbuf(buf, "add@/x", kv3, 2);
    assert(uevent_parse(buf, n3, &ev) == -1);

    /* value containing '=' keeps everything after the first '=' */
    const char *kv4[] = { "ACTION=add", "MODALIAS=usb:v041Ep3272d0100" };
    size_t n4 = mkbuf(buf, "add@/x", kv4, 2);
    assert(uevent_parse(buf, n4, &ev) == 0);
    assert(strcmp(uevent_get(&ev, "MODALIAS"), "usb:v041Ep3272d0100") == 0);

    printf("test_uevent_parse: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_uevent_parse.c -o /tmp/t-parse && /tmp/t-parse`
Expected: FAIL — `schema-udev.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation** — create `schema-udev.h`

```c
#ifndef SCHEMA_UDEV_H
#define SCHEMA_UDEV_H

#include <stddef.h>
#include <string.h>

#define UE_MAX_KEYS 32
#define UE_KEY_MAX  64
#define UE_VAL_MAX  512

struct uevent {
    char key[UE_MAX_KEYS][UE_KEY_MAX];
    char val[UE_MAX_KEYS][UE_VAL_MAX];
    int  n;
};

static inline const char *uevent_get(const struct uevent *ev, const char *key) {
    for (int j = 0; j < ev->n; j++)
        if (strcmp(ev->key[j], key) == 0) return ev->val[j];
    return NULL;
}

/* Parse a raw kernel netlink uevent buffer:
 *   "action@devpath\0KEY=VALUE\0KEY=VALUE\0..."
 * The leading action@devpath record is skipped (redundant with ACTION=/DEVPATH=).
 * Returns 0 if >=1 KEY=VALUE parsed and ACTION present, -1 on malformed. */
static inline int uevent_parse(const char *buf, size_t len, struct uevent *ev) {
    ev->n = 0;
    size_t i = 0;
    while (i < len && buf[i] != '\0') i++;   /* skip header record */
    if (i >= len) return -1;                 /* no NUL -> not a uevent */
    i++;
    while (i < len && ev->n < UE_MAX_KEYS) {
        const char *rec = buf + i;
        size_t rl = strnlen(rec, len - i);
        if (rl == 0) { i++; continue; }
        const char *eq = memchr(rec, '=', rl);
        if (eq) {
            size_t klen = (size_t)(eq - rec);
            size_t vlen = rl - klen - 1;
            if (klen > 0 && klen < UE_KEY_MAX && vlen < UE_VAL_MAX) {
                memcpy(ev->key[ev->n], rec, klen); ev->key[ev->n][klen] = '\0';
                memcpy(ev->val[ev->n], eq + 1, vlen); ev->val[ev->n][vlen] = '\0';
                ev->n++;
            }
        }
        i += rl + 1;
    }
    return uevent_get(ev, "ACTION") ? 0 : -1;
}

#endif /* SCHEMA_UDEV_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_uevent_parse.c -o /tmp/t-parse && /tmp/t-parse`
Expected: PASS — `test_uevent_parse: OK`.

- [ ] **Step 5: Wire into Makefile `test:` target** — add as the last line of the `test:` recipe:

```makefile
	$(CC) $(CFLAGS) tests/test_uevent_parse.c -o /tmp/schema-test-uevent && /tmp/schema-test-uevent
```

- [ ] **Step 6: Run the full suite**

Run: `make test`
Expected: all tests pass, including `test_uevent_parse: OK`.

- [ ] **Step 7: Commit**

```bash
git add schema-udev.h tests/test_uevent_parse.c Makefile
git commit -m "feat(schema-udev): kernel uevent netlink parser + tests"
```

---

### Task 2: .dev rule model + glob match (pure, header + unit test)

**Files:**
- Modify: `schema-udev.h` (append rule model)
- Test: `tests/test_dev_match.c`
- Modify: `Makefile` (add compile line to `test:` target)

**Interfaces:**
- Consumes: `struct uevent`, `uevent_get` (Task 1).
- Produces: `struct dev_rule` with fields `char name[64]; char mkey[8][64]; char mpat[8][512]; int nmatch; char on_add[512]; char on_remove[512];`
- Produces: `int dev_rule_set(struct dev_rule *r, const char *key, const char *val)` → `0` recognised, `-1` unknown key / no match slot. Applies one `key=value` field.
- Produces: `int dev_rule_match(const struct dev_rule *r, const struct uevent *ev)` → `1` match, `0` no.

- [ ] **Step 1: Write the failing test** — `tests/test_dev_match.c`

```c
#include "../schema-udev.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void set(struct uevent *ev, const char *k, const char *v) {
    snprintf(ev->key[ev->n], UE_KEY_MAX, "%s", k);
    snprintf(ev->val[ev->n], UE_VAL_MAX, "%s", v);
    ev->n++;
}

int main(void) {
    /* build a rule: match_subsystem=tty, match_product=10c4/* */
    struct dev_rule r; memset(&r, 0, sizeof r);
    assert(dev_rule_set(&r, "name", "esp32-serial") == 0);
    assert(dev_rule_set(&r, "match_subsystem", "tty") == 0);
    assert(dev_rule_set(&r, "match_product", "10c4/*") == 0);
    assert(dev_rule_set(&r, "on_add", "/usr/local/bin/up.sh") == 0);
    assert(dev_rule_set(&r, "on_remove", "/usr/local/bin/down.sh") == 0);
    assert(r.nmatch == 2);
    assert(strcmp(r.name, "esp32-serial") == 0);
    assert(strcmp(r.on_add, "/usr/local/bin/up.sh") == 0);
    /* match_subsystem stored uppercased as the uevent key SUBSYSTEM */
    assert(strcmp(r.mkey[0], "SUBSYSTEM") == 0);

    /* matching uevent */
    struct uevent ev; ev.n = 0;
    set(&ev, "ACTION", "add"); set(&ev, "SUBSYSTEM", "tty"); set(&ev, "PRODUCT", "10c4/ea60");
    assert(dev_rule_match(&r, &ev) == 1);

    /* wrong product -> no match */
    struct uevent ev2; ev2.n = 0;
    set(&ev2, "SUBSYSTEM", "tty"); set(&ev2, "PRODUCT", "1a86/7523");
    assert(dev_rule_match(&r, &ev2) == 0);

    /* absent key (no PRODUCT) -> no match */
    struct uevent ev3; ev3.n = 0;
    set(&ev3, "SUBSYSTEM", "tty");
    assert(dev_rule_match(&r, &ev3) == 0);

    /* glob ttyUSB* on DEVNAME */
    struct dev_rule r2; memset(&r2, 0, sizeof r2);
    dev_rule_set(&r2, "match_devname", "/dev/ttyUSB*");
    struct uevent ev4; ev4.n = 0; set(&ev4, "DEVNAME", "/dev/ttyUSB0");
    assert(dev_rule_match(&r2, &ev4) == 1);
    struct uevent ev5; ev5.n = 0; set(&ev5, "DEVNAME", "/dev/sda1");
    assert(dev_rule_match(&r2, &ev5) == 0);

    /* rule with zero match conditions matches nothing */
    struct dev_rule r3; memset(&r3, 0, sizeof r3);
    assert(dev_rule_match(&r3, &ev4) == 0);

    /* unknown key rejected */
    struct dev_rule r4; memset(&r4, 0, sizeof r4);
    assert(dev_rule_set(&r4, "bogus", "x") == -1);

    printf("test_dev_match: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_dev_match.c -o /tmp/t-match && /tmp/t-match`
Expected: FAIL — `dev_rule` / `dev_rule_set` undefined.

- [ ] **Step 3: Write minimal implementation** — append to `schema-udev.h` before `#endif`. Add `#include <ctype.h>` and `#include <fnmatch.h>` and `#include <stdio.h>` to the header's include block.

```c
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>

#define RULE_MAX_MATCH 8
#define RULE_HOOK_MAX  512
#define RULE_NAME_MAX  64

struct dev_rule {
    char name[RULE_NAME_MAX];
    char mkey[RULE_MAX_MATCH][UE_KEY_MAX];   /* uevent key, uppercased */
    char mpat[RULE_MAX_MATCH][UE_VAL_MAX];   /* fnmatch pattern */
    int  nmatch;
    char on_add[RULE_HOOK_MAX];
    char on_remove[RULE_HOOK_MAX];
};

/* Apply one key=value field to a rule.
 * Recognised: name, on_add, on_remove, match_<KEY>. Returns 0 ok, -1 unknown/full. */
static inline int dev_rule_set(struct dev_rule *r, const char *key, const char *val) {
    if (strcmp(key, "name") == 0) {
        snprintf(r->name, sizeof r->name, "%s", val);
    } else if (strcmp(key, "on_add") == 0) {
        snprintf(r->on_add, sizeof r->on_add, "%s", val);
    } else if (strcmp(key, "on_remove") == 0) {
        snprintf(r->on_remove, sizeof r->on_remove, "%s", val);
    } else if (strncmp(key, "match_", 6) == 0) {
        const char *sub = key + 6;
        if (*sub == '\0' || r->nmatch >= RULE_MAX_MATCH) return -1;
        int k = r->nmatch;
        size_t z;
        for (z = 0; sub[z] && z < UE_KEY_MAX - 1; z++)
            r->mkey[k][z] = (char)toupper((unsigned char)sub[z]);
        r->mkey[k][z] = '\0';
        snprintf(r->mpat[k], sizeof r->mpat[k], "%s", val);
        r->nmatch++;
    } else {
        return -1;
    }
    return 0;
}

/* Match iff every match_* condition fnmatches the uevent's value for that key. */
static inline int dev_rule_match(const struct dev_rule *r, const struct uevent *ev) {
    if (r->nmatch == 0) return 0;
    for (int k = 0; k < r->nmatch; k++) {
        const char *v = uevent_get(ev, r->mkey[k]);
        if (!v || fnmatch(r->mpat[k], v, 0) != 0) return 0;
    }
    return 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_dev_match.c -o /tmp/t-match && /tmp/t-match`
Expected: PASS — `test_dev_match: OK`.

- [ ] **Step 5: Wire into Makefile `test:` target** — add:

```makefile
	$(CC) $(CFLAGS) tests/test_dev_match.c -o /tmp/schema-test-devmatch && /tmp/schema-test-devmatch
```

- [ ] **Step 6: Commit**

```bash
git add schema-udev.h tests/test_dev_match.c Makefile
git commit -m "feat(schema-udev): .dev rule model + fnmatch glob matching + tests"
```

---

### Task 3: rule file + directory loader (pure, header + unit test)

**Files:**
- Modify: `schema-udev.h` (append loader)
- Modify: `tests/test_dev_match.c` (add loader cases) — OR new `tests/test_dev_load.c`. Use a new file.
- Test: `tests/test_dev_load.c`
- Modify: `Makefile` (add compile line to `test:` target)

**Interfaces:**
- Consumes: `struct dev_rule`, `dev_rule_set` (Task 2).
- Produces: `int dev_rule_load_file(const char *path, struct dev_rule *r)` → `0` ok (rule zeroed then filled), `-1` open failure. Unknown keys warn to stderr, do not fail.
- Produces: `#define MAX_RULES 64`
- Produces: `int dev_rules_load_dir(const char *dir, struct dev_rule *rules, int max)` → count loaded (files ending `.dev`, sorted by name via scandir/alphasort), `>=0`; missing dir → `0`.

- [ ] **Step 1: Write the failing test** — `tests/test_dev_load.c`

```c
#include "../schema-udev.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char tmpl[] = "/tmp/schema-udev-testXXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);

    char p[256];
    snprintf(p, sizeof p, "%s/esp32.dev", dir);
    FILE *f = fopen(p, "w");
    fputs("# a comment line (no '=') is skipped\n", f);
    fputs("name=esp32-serial\n", f);
    fputs("match_subsystem=tty\n", f);
    fputs("match_product=10c4/ea60\n", f);
    fputs("on_add=/bin/true\n", f);
    fputs("bogus_key=ignored-with-warning\n", f);
    fclose(f);

    struct dev_rule r; 
    assert(dev_rule_load_file(p, &r) == 0);
    assert(strcmp(r.name, "esp32-serial") == 0);
    assert(r.nmatch == 2);
    assert(strcmp(r.on_add, "/bin/true") == 0);

    /* a non-.dev file in the dir is ignored by the directory loader */
    snprintf(p, sizeof p, "%s/README.txt", dir);
    f = fopen(p, "w"); fputs("name=notarule\n", f); fclose(f);

    struct dev_rule rules[MAX_RULES];
    int n = dev_rules_load_dir(dir, rules, MAX_RULES);
    assert(n == 1);
    assert(strcmp(rules[0].name, "esp32-serial") == 0);

    /* missing dir -> 0 rules, no crash */
    assert(dev_rules_load_dir("/nonexistent/schema-udev/dir", rules, MAX_RULES) == 0);

    printf("test_dev_load: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_dev_load.c -o /tmp/t-load && /tmp/t-load`
Expected: FAIL — `dev_rule_load_file` / `dev_rules_load_dir` / `MAX_RULES` undefined.

- [ ] **Step 3: Write minimal implementation** — append to `schema-udev.h` before `#endif`. Add `#include <dirent.h>` and `#include <stdlib.h>` to the include block.

```c
#include <dirent.h>
#include <stdlib.h>

#define MAX_RULES 64

/* Load one .dev file into a rule. Mirrors service.c .svc parsing:
 * strchr('='), strip trailing CR/LF, lines without '=' skipped (whole-line
 * comments only). Unknown keys warn but do not fail. Returns 0 ok, -1 open fail. */
static inline int dev_rule_load_file(const char *path, struct dev_rule *r) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(r, 0, sizeof *r);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        if (dev_rule_set(r, line, val) != 0)
            fprintf(stderr, "[schema-udev] %s: ignoring unknown key '%s'\n", path, line);
    }
    fclose(f);
    return 0;
}

static inline int dev_is_dotdev(const struct dirent *d) {
    const char *n = d->d_name;
    size_t l = strlen(n);
    return l > 4 && strcmp(n + l - 4, ".dev") == 0;
}

/* Load all *.dev files from dir (sorted). Returns count, 0 if dir missing. */
static inline int dev_rules_load_dir(const char *dir, struct dev_rule *rules, int max) {
    struct dirent **names = NULL;
    int nf = scandir(dir, &names, dev_is_dotdev, alphasort);
    if (nf < 0) return 0;
    int n = 0;
    for (int i = 0; i < nf; i++) {
        if (n < max) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, names[i]->d_name);
            if (dev_rule_load_file(path, &rules[n]) == 0) n++;
        }
        free(names[i]);
    }
    free(names);
    return n;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -std=c99 -Wall -Wextra -D_GNU_SOURCE tests/test_dev_load.c -o /tmp/t-load && /tmp/t-load`
Expected: PASS — `test_dev_load: OK`.

- [ ] **Step 5: Wire into Makefile `test:` target** — add:

```makefile
	$(CC) $(CFLAGS) tests/test_dev_load.c -o /tmp/schema-test-devload && /tmp/schema-test-devload
```

- [ ] **Step 6: Run full suite + verify -Wall -Wextra clean**

Run: `make test`
Expected: all pass. No compiler warnings from any test compile.

- [ ] **Step 7: Commit**

```bash
git add schema-udev.h tests/test_dev_load.c Makefile
git commit -m "feat(schema-udev): .dev file + directory loader + tests"
```

---

### Task 4: daemon skeleton — netlink open, credential check, receive-and-log loop

**Files:**
- Create: `schema-udev.c`
- Modify: `Makefile` (add `schema-udev` to `BINS`; add build rule)

**Interfaces:**
- Consumes: `schema-udev.h` (`struct uevent`, `uevent_parse`, `uevent_get`).
- Produces (internal, used by Task 5): `static int netlink_open(void)`; `static ssize_t netlink_recv(int fd, char *buf, size_t bufsz)` → datagram length with verified kernel origin, `-1` skip/would-block (sets/keeps `errno`), `-2` fatal; `static volatile sig_atomic_t` not needed (signals via signalfd).

This task's gate is a **live smoke test** (needs root + real netlink), not a pure unit test: run the daemon, emit a synthetic uevent, confirm it is parsed and logged.

- [ ] **Step 1: Write `schema-udev.c` — skeleton that binds netlink, verifies credentials, and logs each parsed uevent**

```c
/* schema-udev — native uevent -> schema rule -> action daemon (Phase 1).
 * Listens on the kernel uevent netlink (group 1), alongside systemd-udevd. */
#define _GNU_SOURCE
#include "schema-udev.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define DEV_DIR "/etc/schema-init/dev"

static int netlink_open(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    int rcv = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;   /* kernel uevents only; NEVER group 2 (udev/libudev) */
    sa.nl_pid    = 0;   /* kernel auto-assigns a unique pid */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}

/* Receive one datagram, verifying it came from the kernel.
 * Returns length (>=0) on a verified event; -1 to skip (spoofed / would-block /
 * ENOBUFS — check errno: EAGAIN/EWOULDBLOCK means drain complete). */
static ssize_t netlink_recv(int fd, char *buf, size_t bufsz) {
    struct iovec iov = { buf, bufsz };
    struct sockaddr_nl sa;
    char cbuf[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msg = { &sa, sizeof sa, &iov, 1, cbuf, sizeof cbuf, 0 };
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0) return -1;                 /* errno set by recvmsg */
    if (sa.nl_pid != 0) { errno = 0; return -1; }   /* not from kernel */
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_CREDENTIALS) { errno = 0; return -1; }
    struct ucred *uc = (struct ucred *)CMSG_DATA(cm);
    if (uc->uid != 0) { errno = 0; return -1; }     /* not root/kernel */
    return n;
}

int main(void) {
    int nlfd = netlink_open();
    if (nlfd < 0) {
        fprintf(stderr, "[schema-udev] netlink open/bind failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");

    for (;;) {
        struct pollfd pfd = { nlfd, POLLIN, 0 };
        if (poll(&pfd, 1, -1) < 0) { if (errno == EINTR) continue; break; }
        for (;;) {
            char buf[8192];
            ssize_t n = netlink_recv(nlfd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;  /* drained */
                if (errno == ENOBUFS) {
                    fprintf(stderr, "[schema-udev] kernel dropped events (ENOBUFS)\n");
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno == 0) continue;   /* spoofed datagram skipped */
                break;                      /* real error */
            }
            struct uevent ev;
            if (uevent_parse(buf, (size_t)n, &ev) != 0) continue;
            /* Task 5 replaces this log with rule matching + hook dispatch. */
            fprintf(stderr, "[schema-udev] uevent %s %s\n",
                    uevent_get(&ev, "ACTION"),
                    uevent_get(&ev, "DEVPATH") ? uevent_get(&ev, "DEVPATH") : "?");
        }
    }
    close(nlfd);
    return 0;
}
```

- [ ] **Step 2: Add `schema-udev` to the Makefile**

Add `schema-udev` to the `BINS ?=` list, and add a build rule after the `schema-journal-sink` rule:

```makefile
schema-udev: schema-udev.c schema-udev.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
```

Also add `schema-udev` to the `rm -f` line in the `clean:` target.

- [ ] **Step 3: Build**

Run: `make schema-udev`
Expected: compiles clean, no `-Wall -Wextra` warnings.

- [ ] **Step 4: Live smoke test — verify it sees a real kernel uevent**

Run (two terminals, or background):
```bash
sudo ./schema-udev &        # prints "listening on kernel uevent netlink (group 1)"
sleep 0.3
# emit a synthetic kernel uevent by retriggering ONE existing device:
d=$(ls -d /sys/bus/usb/devices/*/ | grep -E '/[0-9]+-[0-9]+/$' | head -1)
echo change | sudo tee "$d/uevent"
sleep 0.3
sudo kill %1
```
Expected: schema-udev logs a line like `[schema-udev] uevent change /devices/.../usb1/1-x`. (Using `change`, not `add`, avoids any downstream reprocessing side effects; we only need to prove the netlink path.)

- [ ] **Step 5: Commit**

```bash
git add schema-udev.c Makefile
git commit -m "feat(schema-udev): netlink listener with kernel-credential verification"
```

---

### Task 5: rule dispatch — match, hook exec, SIGCHLD drain, SIGHUP reload

**Files:**
- Modify: `schema-udev.c` (add rule set, signalfd loop, hook exec; replace the Task-4 log line with dispatch)

**Interfaces:**
- Consumes: `dev_rules_load_dir`, `dev_rule_match`, `struct dev_rule`, `MAX_RULES` (Task 3); `netlink_open`, `netlink_recv` (Task 4).
- Produces (internal): `static struct dev_rule g_rules[MAX_RULES]; static int g_nrules;`; `static void rules_reload(void)`; `static void run_hook(const char *hook, const struct uevent *ev)`; `static void dispatch(const struct uevent *ev)`.

Gate: a live integration test firing an `on_add` hook end-to-end, plus a SIGHUP reload check.

- [ ] **Step 1: Add includes and the rule store + reload + hook + dispatch**

Add to the `#include` block of `schema-udev.c`:
```c
#include <sys/signalfd.h>
#include <sys/wait.h>
```

Add above `main()`:
```c
static struct dev_rule g_rules[MAX_RULES];
static int g_nrules = 0;

static void rules_reload(void) {
    struct dev_rule tmp[MAX_RULES];
    int n = dev_rules_load_dir(DEV_DIR, tmp, MAX_RULES);
    memcpy(g_rules, tmp, sizeof(struct dev_rule) * (n < 0 ? 0 : n));
    g_nrules = n < 0 ? 0 : n;
    fprintf(stderr, "[schema-udev] loaded %d rule(s) from %s\n", g_nrules, DEV_DIR);
}

/* Fork a hook, exporting the full uevent (incl. ACTION) as environment.
 * Parent does not block; children are reaped by the SIGCHLD drain in main(). */
static void run_hook(const char *hook, const struct uevent *ev) {
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "[schema-udev] fork: %s\n", strerror(errno)); return; }
    if (pid == 0) {
        for (int j = 0; j < ev->n; j++)
            setenv(ev->key[j], ev->val[j], 1);   /* ACTION included here */
        execl("/bin/sh", "sh", "-c", hook, (char *)NULL);
        _exit(127);
    }
}

static void dispatch(const struct uevent *ev) {
    const char *action = uevent_get(ev, "ACTION");
    if (!action) return;
    for (int i = 0; i < g_nrules; i++) {
        if (!dev_rule_match(&g_rules[i], ev)) continue;
        const char *hook = NULL;
        if (strcmp(action, "add") == 0 && g_rules[i].on_add[0])    hook = g_rules[i].on_add;
        else if (strcmp(action, "remove") == 0 && g_rules[i].on_remove[0]) hook = g_rules[i].on_remove;
        if (hook) {
            fprintf(stderr, "[schema-udev] matched %s %s %s -> %s\n",
                    g_rules[i].name, action,
                    uevent_get(ev, "DEVNAME") ? uevent_get(ev, "DEVNAME") : uevent_get(ev, "DEVPATH"),
                    hook);
            run_hook(hook, ev);
        }
    }
}
```

- [ ] **Step 2: Rewrite `main()` — load rules, add signalfd, dispatch instead of log**

```c
int main(void) {
    int nlfd = netlink_open();
    if (nlfd < 0) {
        fprintf(stderr, "[schema-udev] netlink open/bind failed: %s\n", strerror(errno));
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);  sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);  sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) { fprintf(stderr, "[schema-udev] signalfd: %s\n", strerror(errno)); return 1; }

    rules_reload();
    fprintf(stderr, "[schema-udev] listening on kernel uevent netlink (group 1)\n");

    struct pollfd pfd[2] = { { nlfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
    for (;;) {
        if (poll(pfd, 2, -1) < 0) { if (errno == EINTR) continue; break; }

        if (pfd[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            int quit = 0;
            while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                if (si.ssi_signo == SIGCHLD) {
                    while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* drain all */
                } else if (si.ssi_signo == SIGHUP) {
                    rules_reload();
                } else {
                    quit = 1;   /* TERM / INT */
                }
            }
            if (quit) break;
        }

        if (pfd[0].revents & POLLIN) {
            for (;;) {
                char buf[8192];
                ssize_t n = netlink_recv(nlfd, buf, sizeof buf);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == ENOBUFS) {
                        fprintf(stderr, "[schema-udev] kernel dropped events (ENOBUFS)\n");
                        continue;
                    }
                    if (errno == EINTR) continue;
                    if (errno == 0) continue;   /* spoofed datagram skipped */
                    break;
                }
                struct uevent ev;
                if (uevent_parse(buf, (size_t)n, &ev) != 0) continue;
                dispatch(&ev);
            }
        }
    }
    fprintf(stderr, "[schema-udev] shutting down\n");
    close(nlfd); close(sfd);
    return 0;
}
```

Delete the old Task-4 `main()`.

- [ ] **Step 3: Build**

Run: `make schema-udev`
Expected: clean, no warnings.

- [ ] **Step 4: Live integration test — hook fires end-to-end (no unplug)**

```bash
sudo mkdir -p /etc/schema-init/dev
# match a device known present: retrigger emits SUBSYSTEM=usb on 'change'; use a
# transient rule keyed on the harmless 'change' path via a wildcard product.
d=$(ls -d /sys/bus/usb/devices/*/ | grep -E '/[0-9]+-[0-9]+/$' | head -1)
prod=$(sed -n 's/^PRODUCT=//p' "$d/uevent")
printf 'name=phase1-selftest\nmatch_subsystem=usb\nmatch_product=%s\non_add=/usr/bin/touch /tmp/schema-udev-marker\n' "$prod" \
  | sudo tee /etc/schema-init/dev/selftest.dev
rm -f /tmp/schema-udev-marker
sudo ./schema-udev &
sleep 0.3
echo add | sudo tee "$d/uevent"      # synthetic 'add' on group 1
sleep 0.4
sudo kill %1
ls -l /tmp/schema-udev-marker        # MUST exist
sudo rm /etc/schema-init/dev/selftest.dev
```
Expected: `/tmp/schema-udev-marker` exists, and the log shows `matched phase1-selftest add ... -> /usr/bin/touch ...`.

- [ ] **Step 5: SIGHUP reload check**

```bash
sudo ./schema-udev &     # logs "loaded 0 rule(s)"
sleep 0.2
printf 'name=r\nmatch_subsystem=usb\non_add=/bin/true\n' | sudo tee /etc/schema-init/dev/r.dev
sudo kill -HUP %1
sleep 0.2                # log must show "loaded 1 rule(s)"
sudo kill %1
sudo rm /etc/schema-init/dev/r.dev
```
Expected: after HUP, log shows `loaded 1 rule(s)`.

- [ ] **Step 6: Commit**

```bash
git add schema-udev.c
git commit -m "feat(schema-udev): rule dispatch, hook exec, SIGCHLD drain, SIGHUP reload"
```

---

### Task 6: packaging — service unit, install, inert example, README, vmtest

**Files:**
- Create: `services/schema-udev.svc`
- Create: `distros/fedora-kde/services/schema-udev.svc`
- Create: `assets/example.dev` (inert)
- Modify: `setup.sh` (create `/etc/schema-init/dev`, install example inert)
- Modify: `README.md` (schema-udev section)

**Interfaces:** none (packaging). Consumes the built `schema-udev` binary.

- [ ] **Step 1: Create `services/schema-udev.svc`**

```
name=schema-udev
exec=/usr/bin/schema-udev
needs_root=1
```

(No `critical`, no `dep` — independent kernel listener; if it dies real udevd still runs.)

- [ ] **Step 2: Create `distros/fedora-kde/services/schema-udev.svc`** — identical content to Step 1.

- [ ] **Step 3: Create `assets/example.dev` — fully inert (all lines commented)**

```
# schema-udev rule example (INERT — every line commented so it loads no rule).
# Copy to /etc/schema-init/dev/<name>.dev and uncomment when the hardware is present.
#
# Match keys map to raw kernel uevent keys (match_subsystem -> SUBSYSTEM), ANDed,
# fnmatch globs allowed. Hooks run /bin/sh -c with the full uevent exported as env
# (ACTION, DEVNAME, DEVPATH, PRODUCT, MODALIAS, ...). Comments must be on their own
# line — inline comments after a value are NOT stripped.
#
# name=esp32-serial
# match_subsystem=tty
# match_product=10c4/*
# on_add=/usr/local/bin/esp32-up.sh
# on_remove=/usr/local/bin/esp32-down.sh
```

- [ ] **Step 4: Update `setup.sh`** — where it installs services/config, add creation of the rules dir and the inert example. Locate the block that creates `/etc/schema-init/services` and add alongside it:

```sh
install -d "$DESTDIR/etc/schema-init/dev"
install -m 0644 assets/example.dev "$DESTDIR/etc/schema-init/dev/example.dev"
```

(Verify the exact variable/idiom `setup.sh` already uses for the services dir and match it; the example stays inert so a boot with it present loads zero rules.)

- [ ] **Step 5: `make install` picks up the new .svc automatically**

The Makefile `install:` target already copies `services/*` to the data dir and `BINS` to `BINDIR`; `schema-udev` (added to `BINS` in Task 4) and `services/schema-udev.svc` are installed with no Makefile change. Verify:

Run: `make && make install DESTDIR=/tmp/su-stage && ls /tmp/su-stage/usr/bin/schema-udev /tmp/su-stage/usr/share/schema-init/services/schema-udev.svc`
Expected: both paths exist.

- [ ] **Step 6: Add a README section**

Add a `## schema-udev` section to `README.md` documenting: what it is (native uevent→schema→action, alongside udevd), the `.dev` grammar (match_* ANDed globs, on_add/on_remove, own-line comments only), that it binds kernel netlink group 1, and the Phase 1 boundaries (no coldplug / symlinks / db yet).

- [ ] **Step 7: Full build + test**

Run: `make clean && make && make test`
Expected: binaries build clean; all unit tests pass (`test_uevent_parse`, `test_dev_match`, `test_dev_load` among them).

- [ ] **Step 8: vmtest as PID 1**

Invoke the `schema-vmtest` skill (QEMU/KVM boot with `schema-udev.svc` in the rail). Confirm: boots as PID 1, `schema-udev` supervises clean (state reaches PERFECT/FUNDAMENTAL), logs `listening on kernel uevent netlink (group 1)` and `loaded N rule(s)`, no regression in the existing rail.

- [ ] **Step 9: Commit**

```bash
git add services/schema-udev.svc distros/fedora-kde/services/schema-udev.svc assets/example.dev setup.sh README.md
git commit -m "feat(schema-udev): service unit, install wiring, inert example rule, README"
```

---

## Definition of done (Phase 1)

- `make clean && make && make test` green (parse + match + load unit tests pass, no `-Wall -Wextra` warnings).
- `schema-udev` binds netlink group 1, drops spoofed datagrams (cred/pid check), parses real kernel uevents.
- Live synthetic-uevent integration test fires an `on_add` hook end-to-end; `SIGHUP` reloads rules.
- `schema-udev.svc` supervises clean under vmtest as PID 1, no regression.
- Runs alongside `udevd` with zero observed impact on PipeWire / desktop hotplug.
- Deploy to live box is reboot-gated per schema-init convention (new supervised service enters the rail on `schema-ctl reload`, but a `reboot` is the clean validation — coordinate with Jonathan; do NOT deploy without his go).

## Self-review notes

- **Spec coverage:** netlink group-1 (T4), cred check (T4), parse (T1), .dev grammar + match_* AND + fnmatch (T2), rule loading + reload (T3/T5), fork/exec with ACTION exported (T5), SIGCHLD drain (T5), SIGHUP reload (T5), not-critical/no-dep svc (T6), inert example (T6), no-coldplug boundary (respected — no /sys replay anywhere), tests incl. live integration (T4/T5) + vmtest (T6). All spec sections mapped.
- **Type consistency:** `struct uevent`, `struct dev_rule`, `uevent_get`, `uevent_parse`, `dev_rule_set`, `dev_rule_match`, `dev_rule_load_file`, `dev_rules_load_dir`, `MAX_RULES`, `netlink_open`, `netlink_recv`, `rules_reload`, `run_hook`, `dispatch` — names and signatures consistent across tasks.
- **Parse parity:** T3 loader mirrors `service_load_one` (`strchr('=')`, `strcspn` CR/LF strip, no-`=` lines skipped) — no inline comment stripping; example.dev uses own-line comments accordingly.
