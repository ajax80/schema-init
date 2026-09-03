# schema-dbus SP1 Broker (v1.0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `schema-dbus`, a wire-compatible D-Bus **system-bus** broker in C that replaces `dbus-daemon --system`, owning the event loop, connections, auth, name registry, routing, reply-tracking, and policy — with libdbus linked only as the wire codec.

**Architecture:** Single-threaded `epoll` broker. libdbus does marshal/demarshal of message bytes; everything else is ours. Policy is a line-for-line C port of `tools/dbus-learn/felt_policy.py`, proven byte-identical against the 14,979-message SP0 corpus via a dual-engine conformance gate. Service activation is deferred to v1.1.

**Tech Stack:** C99, libdbus (`dbus-1`, codec only), Linux `epoll`/`SO_PEERCRED`/`getgrouplist`/`recvmsg`/`sendmsg`+`SCM_RIGHTS`, Python 3 (existing `felt_policy.py`/`dissect_policy.py` reused for the golden oracle).

**Spec:** `docs/superpowers/specs/2026-09-02-schema-dbus-sp1-broker-design.md`

## Global Constraints

- **C standard/flags** (verbatim from Makefile): `-std=c99 -Wall -Wextra -D_GNU_SOURCE -I.`. New warnings are failures.
- **Test style:** house convention — each unit test is `tests/test_<mod>.c` with `#include "../<mod>.h"`, `<assert.h>`, a `main()` of `assert(...)`, ending `printf("all <mod> tests passed\n"); return 0;`. Wire each into the Makefile `test:` target.
- **Source layout:** implementation-in-header pattern (like `path_id.h`, `cdrom_id.h`): each module is `sdbus_<mod>.h` carrying its implementation, `#include`d by both the binary TU `schema-dbus.c` and its test. Only `schema-dbus.c` has `main()` for the daemon.
- **Build dep:** `dbus-devel` must be installed (`pkg-config --exists dbus-1`); link flags come from `pkg-config --cflags --libs dbus-1`.
- **Policy source of truth:** never parse `system.conf` XML in C. The dissolved policy text (output of `tools/dbus-learn/dissect_policy.py:dissolve_tree`) is the only policy input `sdbus_policy.h` reads; the C parser must accept the exact format `felt_policy.py:parse_policy` accepts.
- **gids fidelity:** the broker derives supplementary groups via `getgrouplist(uid)`, which equals dbus `GetConnectionCredentials.UnixGroupIDs` (`schema_dbus_learn.py:96-99`) — the corpus derivation. Do not change this without re-baselining the corpus.
- **Cutover:** reboot-only. Never hot-swap the live system bus.
- **Commits:** end every commit message body with the two standard trailers (Co-Authored-By + Claude-Session) already used on this branch.
- **Deferred to v1.1 (do NOT implement):** `StartServiceByName`, `.service` parsing, activation queue. `StartServiceByName` returns `org.freedesktop.DBus.Error.ServiceUnknown`.

---

## File Structure

Created by this plan (top-level unless noted):

- `sdbus_policy.h` — parsed dissolved policy + `sdbus_req` + evaluator (C port of felt_policy).
- `sdbus_names.h` — name registry: unique-name allocation, well-known ownership + queues, RequestName/ReleaseName semantics, transition events.
- `sdbus_match.h` — AddMatch rule parse + signal matching.
- `sdbus_reply.h` — pending-reply table (serial↔caller/callee), purge-on-disconnect.
- `sdbus_codec.h` — libdbus wrapper: demarshal/marshal, header-field extraction, SENDER stamping, fd list access.
- `sdbus_auth.h` — SASL EXTERNAL handshake state machine incl. NEGOTIATE_UNIX_FD.
- `sdbus_conn.h` — per-connection struct + registry of connections; SO_PEERCRED/getgrouplist capture; in/out buffers with cmsg fd queues.
- `sdbus_route.h` — `deliver()`: unicast/broadcast/directed-signal/reply routing + policy gate call.
- `sdbus_driver.h` — `org.freedesktop.DBus` method dispatch.
- `schema-dbus.c` — `main()`: listen socket, epoll loop, `recvmsg`/`sendmsg` with SCM_RIGHTS, wiring.
- `tools/dbus-learn/emit_conformance_golden.py` — emits (request, expected-verdict) golden from corpus via felt_policy.
- `tests/test_sdbus_policy.c`, `tests/test_sdbus_names.c`, `tests/test_sdbus_match.c`, `tests/test_sdbus_reply.c`, `tests/test_sdbus_codec.c`, `tests/test_sdbus_auth.c`, `tests/test_sdbus_conformance.c` — unit + conformance tests.
- `tests/sdbus_live_interop.sh` — live scratch-socket interop test.
- `services/dbus.svc` — modified at cutover (Task 13).

Modified: `Makefile` (BINS + `test:` target), `docs/superpowers/specs/...` (none).

---

## Task 1: Build scaffolding — `schema-dbus` binary links libdbus

**Files:**
- Create: `schema-dbus.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable `schema-dbus` binary; Makefile var `DBUS_CFLAGS`/`DBUS_LIBS` from pkg-config.

- [ ] **Step 1: Install the build dep and confirm pkg-config sees it**

Run:
```bash
sudo dnf install -y dbus-devel
pkg-config --exists dbus-1 && pkg-config --modversion dbus-1
```
Expected: prints a version (e.g. `1.14.x`). If this fails, stop — nothing else compiles.

- [ ] **Step 2: Write the minimal daemon TU**

Create `schema-dbus.c`:
```c
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int system_bus = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--system")) system_bus = 1;
    /* Prove libdbus links: ask it for the system bus address it would use. */
    const char *addr = dbus_bus_get_type ? "linked" : "linked";
    fprintf(stderr, "schema-dbus: starting (system=%d, libdbus %s)\n",
            system_bus, addr);
    return 0;
}
```

- [ ] **Step 3: Add Makefile rule and BINS entry**

In `Makefile`, after the other pkg-config-free rules, add near the top (after CFLAGS):
```make
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)
```
Append `schema-dbus` to `BINS`:
```make
BINS ?= schema-init schema-ctl schema-subreaper schema-journal-sink schema-board schema-udev schema-dbus
```
Add the build rule:
```make
schema-dbus: schema-dbus.c sdbus_policy.h sdbus_names.h sdbus_match.h sdbus_reply.h sdbus_codec.h sdbus_auth.h sdbus_conn.h sdbus_route.h sdbus_driver.h
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) schema-dbus.c -o $@ $(DBUS_LIBS)
```
(The header deps don't exist yet; create empty stubs in Step 4 so the build works incrementally.)

- [ ] **Step 4: Create empty header stubs so the link succeeds**

Create each of `sdbus_policy.h sdbus_names.h sdbus_match.h sdbus_reply.h sdbus_codec.h sdbus_auth.h sdbus_conn.h sdbus_route.h sdbus_driver.h` containing only an include guard, e.g. `sdbus_policy.h`:
```c
#ifndef SDBUS_POLICY_H
#define SDBUS_POLICY_H
#endif
```

- [ ] **Step 5: Build and run**

Run: `make schema-dbus && ./schema-dbus --system`
Expected: prints `schema-dbus: starting (system=1, libdbus linked)`, exits 0.

- [ ] **Step 6: Commit**

```bash
git add schema-dbus.c Makefile sdbus_*.h
git commit -m "feat(schema-dbus): build scaffolding links libdbus"
```

---

## Task 2: Policy port + conformance gate (the crown)

Port `felt_policy.py` to C and prove it byte-identical against the corpus. This task has no socket code — it is a pure function over request tuples, so it is fully unit-testable now and is the highest-risk piece, done first.

**Files:**
- Create: `sdbus_policy.h`, `tests/test_sdbus_policy.c`, `tests/test_sdbus_conformance.c`
- Create: `tools/dbus-learn/emit_conformance_golden.py`
- Modify: `tools/dbus-learn/felt_policy.py` (extract a shared `record_to_request`), `Makefile` (`test:`)

**Interfaces:**
- Consumes: dissolved policy text; corpus JSONL at `tests/dbus-corpus/capture-20260902.jsonl`.
- Produces:
  ```c
  typedef struct {
      const char *op;          /* "send" | "own" | NULL */
      int uid;                 /* -1 == unknown */
      const int *gids; int n_gids;
      const char *interface, *member, *msgtype, *path;
      const char *destination;
      const char **dest_names; int n_dest_names;
      const char *name;                 /* for op=="own" */
      int has_reply_serial;             /* 0/1 */
  } sdbus_req;
  typedef struct sdbus_policy sdbus_policy;
  sdbus_policy *sdbus_policy_parse(const char *dissolved_text);
  const char   *sdbus_policy_eval(const sdbus_policy *, const sdbus_req *); /* "allow"|"deny" */
  void          sdbus_policy_free(sdbus_policy *);
  ```

- [ ] **Step 1: Write the failing unit test for the evaluator**

Create `tests/test_sdbus_policy.c` — mirrors felt_policy's own semantics with small hand-built policies:
```c
#include "../sdbus_policy.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static const char *evals(const char *pol, sdbus_req *r) {
    sdbus_policy *p = sdbus_policy_parse(pol);
    const char *v = sdbus_policy_eval(p, r);
    sdbus_policy_free(p);
    return v;
}

int main(void) {
    /* default deny, mandatory allow-all send is overridden by later deny */
    const char *names[] = {"org.example.Svc"};
    sdbus_req send = { .op="send", .uid=1000, .interface="org.example.If",
        .member="Do", .msgtype="method_call", .destination=":1.5",
        .dest_names=names, .n_dest_names=1 };

    /* empty policy -> deny (start verdict) */
    assert(strcmp(evals("", &send), "deny") == 0);

    /* default context allowing this destination -> allow */
    const char *p1 = "context = default\nallow = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p1, &send), "allow") == 0);

    /* send_destination is set-membership: different name -> deny */
    const char *p2 = "context = default\nallow = send_destination:org.other\n";
    assert(strcmp(evals(p2, &send), "deny") == 0);

    /* last-match-wins across ordered contexts: default allow, user deny */
    const char *p3 = "context = default\nallow = send_destination:org.example.Svc\n"
                     "context = user:1000\ndeny = send_destination:org.example.Svc\n";
    assert(strcmp(evals(p3, &send), "deny") == 0);

    /* requested-reply exemption: reply short-circuits to allow even with no rule */
    sdbus_req reply = { .op="send", .uid=1000, .msgtype="method_return",
        .has_reply_serial=1, .destination=":1.9" };
    assert(strcmp(evals("", &reply), "allow") == 0);

    /* own gating: own rule matches only op=="own" */
    sdbus_req own = { .op="own", .uid=0, .name="org.example.Svc" };
    const char *p4 = "context = default\nallow = own:org.example.Svc\n";
    assert(strcmp(evals(p4, &own), "allow") == 0);
    const char *p5 = "context = default\nallow = own:org.other\n";
    assert(strcmp(evals(p5, &own), "deny") == 0);

    /* own_prefix */
    const char *p6 = "context = default\nallow = own_prefix:org.example\n";
    assert(strcmp(evals(p6, &own), "allow") == 0);

    printf("all sdbus_policy tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cc -std=c99 -D_GNU_SOURCE -I. tests/test_sdbus_policy.c -o /tmp/t_pol`
Expected: FAIL to compile (`sdbus_policy_parse` undefined).

- [ ] **Step 3: Implement `sdbus_policy.h`**

Write the evaluator as a direct translation of `felt_policy.py`. Structure:
- Parse: split into contexts (`context = kind:selector`) each holding rules (`allow = pred:val, pred:val` / `deny = ...`), exactly as `parse_policy` + `_parse_predicates` (comma-split, `:`-partition).
- Store contexts in four buckets by kind. `evaluate` iterates default→group→user→mandatory, last-match-wins, start `deny`; `_is_requested_reply` short-circuits first.
- `_rule_matches`: replicate the predicate switch — `user`/`group` bare `*` gate; `own`/`own_prefix` require `op=="own"`; `send_destination` set-membership over `dest_names` (fallback `destination`); `_SEND`/`_RECV` scalar map via `_field_matches` (`*` == non-NULL, else exact string equal).
- `_applicable`: default/mandatory always; user by uid (numeric selector via `atoi`, name selector via `getpwnam`); group by gid membership (numeric via `atoi`, name via `getgrnam`), against `r->gids`.
Cache `getpwnam`/`getgrnam` results in a small static array (selectors are few). Represent verdict as the interned literals `"allow"`/`"deny"` so callers can `strcmp` or pointer-compare.

Keep every predicate/branch traceable to a `felt_policy.py` line number in a comment (per house terseness rules, comments only where they pin fidelity).

- [ ] **Step 4: Run the unit test to verify it passes**

Run: `cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_sdbus_policy.c -o /tmp/t_pol && /tmp/t_pol`
Expected: `all sdbus_policy tests passed`.

- [ ] **Step 5: Factor a shared `record_to_request` in Python**

In `tools/dbus-learn/verify_dbus_policy_live.py` the per-record request dict is built inline (the block ending near line 77 with `msgtype`/`path`/`reply_serial`). Extract it into `felt_policy.py` as `record_to_request(r) -> dict` and import it in `verify_dbus_policy_live.py`, replacing the inline construction. Run the existing gate to confirm no behavior change:
```bash
cd tools/dbus-learn && python3 verify_dbus_policy_live.py \
  ../../tests/dbus-corpus/capture-20260902.jsonl ../../tests/dbus-corpus/rejections-20260902.log
```
Expected: `false_positives=0 false_negatives=0` (unchanged from the SP0 close).

- [ ] **Step 6: Write the golden emitter**

Create `tools/dbus-learn/emit_conformance_golden.py`:
```python
#!/usr/bin/env python3
"""Emit (request, expected-verdict) golden for the C policy conformance gate."""
import json, sys
from felt_policy import parse_policy, evaluate, record_to_request
from dissect_policy import dissolve_tree

def main():
    corpus = sys.argv[1]
    dissolved = dissolve_tree("/usr/share/dbus-1/system.conf")
    policy = parse_policy(dissolved)
    # Emit the dissolved policy and one golden line per adjudicable record.
    with open(sys.argv[2], "w") as pol:
        pol.write(dissolved)
    with open(corpus) as f, open(sys.argv[3], "w") as out:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            req = record_to_request(r)
            if req.get("uid") is None:      # un-adjudicable: skipped in the gate
                continue
            verdict = evaluate(policy, req)
            out.write(json.dumps({"req": req, "verdict": verdict}) + "\n")

if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Write the failing conformance test (C)**

Create `tests/test_sdbus_conformance.c`: reads the dissolved policy file and the golden JSONL (paths from argv), builds an `sdbus_req` from each `req`, calls `sdbus_policy_eval`, asserts it equals `verdict`; counts and prints total. Use a tiny hand-rolled JSON field reader is error-prone — instead have the emitter also write a **flat** golden (`tests/dbus-corpus/policy-golden.tsv`) with tab-separated primitive columns the C test parses with `strtok`: `verdict<TAB>op<TAB>uid<TAB>gids(csv)<TAB>interface<TAB>member<TAB>msgtype<TAB>path<TAB>destination<TAB>dest_names(csv)<TAB>name<TAB>has_reply_serial`. Add that TSV write to the emitter (Step 6) alongside the JSONL. The C test:
```c
#include "../sdbus_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* read dissolved policy file -> sdbus_policy_parse; for each TSV row build
   sdbus_req, eval, assert == column 0. Fail loudly on first mismatch with the
   row printed. Print "conformance: N/N verdicts matched". */
```
Fill in the reader concretely (split each line on `\t`, split csv columns on `,`, map empty string to NULL, `has_reply_serial` via `atoi`).

- [ ] **Step 8: Generate golden and run conformance**

Run:
```bash
cd tools/dbus-learn && python3 emit_conformance_golden.py \
  ../../tests/dbus-corpus/capture-20260902.jsonl \
  ../../tests/dbus-corpus/policy-dissolved.txt \
  ../../tests/dbus-corpus/policy-golden.tsv && cd ../..
cc -std=c99 -Wall -Wextra -D_GNU_SOURCE -I. tests/test_sdbus_conformance.c -o /tmp/t_conf
/tmp/t_conf tests/dbus-corpus/policy-dissolved.txt tests/dbus-corpus/policy-golden.tsv
```
Expected: `conformance: <N>/<N> verdicts matched` where N ≈ 14,940 (14,979 minus the 39 uid-null skips). **Any mismatch fails the build** — fix `sdbus_policy.h` until zero divergence.

- [ ] **Step 9: Wire both tests into the Makefile `test:` target**

Add:
```make
	$(CC) $(CFLAGS) tests/test_sdbus_policy.c -o /tmp/schema-test-sdbus-policy && /tmp/schema-test-sdbus-policy
	$(CC) $(CFLAGS) tests/test_sdbus_conformance.c -o /tmp/schema-test-sdbus-conf && /tmp/schema-test-sdbus-conf tests/dbus-corpus/policy-dissolved.txt tests/dbus-corpus/policy-golden.tsv
```
Commit the dissolved+golden fixtures (they are derived but stable; committing them makes `make test` hermetic without a live `system.conf`).

- [ ] **Step 10: Commit**

```bash
git add sdbus_policy.h tests/test_sdbus_policy.c tests/test_sdbus_conformance.c \
  tools/dbus-learn/emit_conformance_golden.py tools/dbus-learn/felt_policy.py \
  tools/dbus-learn/verify_dbus_policy_live.py tests/dbus-corpus/policy-dissolved.txt \
  tests/dbus-corpus/policy-golden.tsv Makefile
git commit -m "feat(schema-dbus): C policy port proven byte-identical on 14979-msg corpus"
```

---

## Task 3: Codec wrapper — demarshal/marshal + SENDER stamping

**Files:**
- Create: `sdbus_codec.h`, `tests/test_sdbus_codec.c`
- Modify: `Makefile` (`test:`)

**Interfaces:**
- Consumes: libdbus.
- Produces:
  ```c
  typedef struct {
      DBusMessage *msg;        /* owned */
      int type;                /* DBUS_MESSAGE_TYPE_* */
      dbus_uint32_t serial, reply_serial; int has_reply_serial;
      const char *destination, *path, *interface, *member, *signature, *error_name;
  } sdbus_msg;
  /* demarshal one complete message from buf[0..len); returns bytes consumed
     (>0), 0 if a full message is not yet buffered, -1 on protocol error. */
  int  sdbus_codec_take(const unsigned char *buf, int len, sdbus_msg *out);
  void sdbus_msg_free(sdbus_msg *);
  /* stamp the verified unique sender, then marshal to a fresh malloc'd buffer;
     caller frees *bytes. */
  int  sdbus_codec_emit(sdbus_msg *, const char *sender_unique,
                        unsigned char **bytes, int *len);
  int  sdbus_msg_n_fds(const sdbus_msg *);   /* unix fds carried */
  ```

- [ ] **Step 1: Write the failing round-trip test**

Create `tests/test_sdbus_codec.c`: build a `method_call` with libdbus (`dbus_message_new_method_call`), marshal it with `dbus_message_marshal`, feed the bytes to `sdbus_codec_take`, assert extracted `type`/`member`/`interface`/`path`/`destination` match; assert `sdbus_codec_take` on a truncated buffer returns 0; then `sdbus_codec_emit` with `sender_unique=":1.7"`, re-demarshal, assert the SENDER field equals `:1.7`.

- [ ] **Step 2: Run to verify it fails**

Run: `cc -std=c99 -D_GNU_SOURCE -I. $(pkg-config --cflags dbus-1) tests/test_sdbus_codec.c -o /tmp/t_codec $(pkg-config --libs dbus-1)`
Expected: FAIL (undefined `sdbus_codec_take`).

- [ ] **Step 3: Implement `sdbus_codec.h`**

- `sdbus_codec_take`: use `dbus_message_demarshal_bytes_needed(buf,len)`; if it returns >len or <=0 handle (0 → need more; <0 → error). Else `dbus_message_demarshal(buf, needed)`, populate `sdbus_msg` from `dbus_message_get_*` accessors (`type`, `serial`, `reply_serial` via `dbus_message_get_reply_serial` — 0 means none, set `has_reply_serial` accordingly), return `needed`.
- `sdbus_codec_emit`: `dbus_message_set_sender(msg, sender_unique)`, then `dbus_message_marshal` to `bytes`/`len`.
- `sdbus_msg_n_fds`: `dbus_message_get_unix_fds` if available (guard with `#ifdef DBUS_TYPE_UNIX_FD`); the actual fd relay is Task 9 — here just report the count.

- [ ] **Step 4: Run to verify it passes**

Run the same cc line + `/tmp/t_codec`. Expected: `all sdbus_codec tests passed`.

- [ ] **Step 5: Wire into Makefile `test:`** (with `$(DBUS_CFLAGS)`/`$(DBUS_LIBS)`), then commit.

```bash
git add sdbus_codec.h tests/test_sdbus_codec.c Makefile
git commit -m "feat(schema-dbus): libdbus codec wrapper with sender stamping"
```

---

## Task 4: Name registry — RequestName/ReleaseName/queues/transitions

**Files:**
- Create: `sdbus_names.h`, `tests/test_sdbus_names.c`
- Modify: `Makefile` (`test:`)

**Interfaces:**
- Consumes: nothing (pure logic; connections referred to by opaque `int conn_id`).
- Produces:
  ```c
  #define SDBUS_REQ_ALLOW_REPLACEMENT 0x1
  #define SDBUS_REQ_REPLACE_EXISTING  0x2
  #define SDBUS_REQ_DO_NOT_QUEUE      0x4
  enum { SDBUS_REQ_PRIMARY_OWNER=1, SDBUS_REQ_IN_QUEUE=2, SDBUS_REQ_EXISTS=3, SDBUS_REQ_ALREADY_OWNER=4 };
  enum { SDBUS_REL_RELEASED=1, SDBUS_REL_NON_EXISTENT=2, SDBUS_REL_NOT_OWNER=3 };
  typedef struct sdbus_names sdbus_names;
  typedef struct { const char *name; int old_owner, new_owner; } sdbus_transition; /* -1 == none */
  sdbus_names *sdbus_names_new(void);
  const char *sdbus_names_alloc_unique(sdbus_names *, int conn_id);  /* ":1.N" */
  int  sdbus_names_request(sdbus_names *, int conn_id, const char *well_known, unsigned flags,
                           sdbus_transition *out, int *n_out);       /* returns SDBUS_REQ_* */
  int  sdbus_names_release(sdbus_names *, int conn_id, const char *well_known,
                           sdbus_transition *out, int *n_out);       /* returns SDBUS_REL_* */
  int  sdbus_names_owner(sdbus_names *, const char *name);           /* conn_id or -1 */
  void sdbus_names_disconnect(sdbus_names *, int conn_id, sdbus_transition *out, int *n_out);
  int  sdbus_names_list(sdbus_names *, const char ***out_names);     /* all owned names; caller frees array */
  void sdbus_names_free(sdbus_names *);
  ```

- [ ] **Step 1: Write the failing test** — cover: first requester becomes PRIMARY_OWNER (transition none→c1); second requester with DO_NOT_QUEUE → EXISTS, no transition; second requester without flags → IN_QUEUE; owner ReleaseName → next in queue promoted (transition c1→c2); REPLACE_EXISTING on an ALLOW_REPLACEMENT owner → new owner takes over (transition + old owner gets no ownership); `sdbus_names_owner` reflects current owner; `sdbus_names_disconnect` of the owner promotes the queue; re-request by current owner → ALREADY_OWNER. Assert exact transition tuples.

- [ ] **Step 2: Run to verify it fails** (compile error, undefined symbols).

- [ ] **Step 3: Implement `sdbus_names.h`** — arrays/linked lists: a monotonic `next_unique`, a table of `{well_known, owner_conn, queue[]}`. Implement the flag semantics exactly per the D-Bus spec (ALLOW_REPLACEMENT stored on the current owner; REPLACE_EXISTING honored only if current owner allowed replacement; DO_NOT_QUEUE controls EXISTS vs IN_QUEUE). Every owner change appends a `sdbus_transition`. `alloc_unique` formats `":1.%u"`.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Wire into `test:`, commit.**
```bash
git add sdbus_names.h tests/test_sdbus_names.c Makefile
git commit -m "feat(schema-dbus): name registry with ownership queues and transitions"
```

---

## Task 5: Match rules — AddMatch parse + signal matching

**Files:**
- Create: `sdbus_match.h`, `tests/test_sdbus_match.c`
- Modify: `Makefile` (`test:`)

**Interfaces:**
- Produces:
  ```c
  typedef struct sdbus_matchset sdbus_matchset;
  sdbus_matchset *sdbus_match_new(void);
  int  sdbus_match_add(sdbus_matchset *, const char *rule);   /* parses "type='signal',interface='..',..." */
  int  sdbus_match_remove(sdbus_matchset *, const char *rule);/* exact-string removal, per spec */
  int  sdbus_match_signal(sdbus_matchset *, const char *interface, const char *member,
                          const char *path, const char *sender); /* 1 if any rule matches */
  void sdbus_match_free(sdbus_matchset *);
  ```

- [ ] **Step 1: Write the failing test** — parse a rule `type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'`; assert a matching signal returns 1, a different-interface signal returns 0; a `type='signal'` bare rule matches any signal; `sdbus_match_remove` of the exact string stops matching; a malformed rule returns -1 from add.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sdbus_match.h`** — parse the comma-separated `key='value'` grammar (values single-quoted, commas only outside quotes) into a struct of optional constraints (type, interface, member, path, sender, path_namespace). `sdbus_match_signal` returns 1 if ANY stored rule's present constraints all equal the given fields (absent constraint = wildcard). Store the raw rule string for exact-match removal.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Wire into `test:`, commit.**
```bash
git add sdbus_match.h tests/test_sdbus_match.c Makefile
git commit -m "feat(schema-dbus): match-rule parse and signal matching"
```

---

## Task 6: Pending-reply table

**Files:**
- Create: `sdbus_reply.h`, `tests/test_sdbus_reply.c`
- Modify: `Makefile` (`test:`)

**Interfaces:**
- Produces:
  ```c
  typedef struct sdbus_replies sdbus_replies;
  sdbus_replies *sdbus_replies_new(void);
  /* record that caller sent a call (serial) to callee awaiting a reply */
  void sdbus_replies_record(sdbus_replies *, int callee, dbus_uint32_t serial, int caller);
  /* a reply from `from` with reply_serial: returns caller conn_id to deliver to,
     consuming the entry; -1 if no match (drop the reply). */
  int  sdbus_replies_match(sdbus_replies *, int from, dbus_uint32_t reply_serial);
  /* purge every entry where conn is caller or callee (on disconnect) */
  void sdbus_replies_purge(sdbus_replies *, int conn);
  void sdbus_replies_free(sdbus_replies *);
  ```

- [ ] **Step 1: Write the failing test** — record (callee=2, serial=42, caller=1); `match(from=2, reply_serial=42)` returns 1 and consumes (second match returns -1); a reply from the wrong `from` returns -1; `purge(1)` removes entries where 1 is caller; `purge(2)` removes where 2 is callee.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sdbus_reply.h`** — a growable array of `{callee, serial, caller, live}`. `match` scans for a live entry with `callee==from && serial==reply_serial`, marks it dead, returns caller. `purge` marks dead all entries touching `conn`.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Wire into `test:`, commit.**
```bash
git add sdbus_reply.h tests/test_sdbus_reply.c Makefile
git commit -m "feat(schema-dbus): pending-reply table with disconnect purge"
```

---

## Task 7: Connection state + SASL EXTERNAL auth

**Files:**
- Create: `sdbus_conn.h`, `sdbus_auth.h`, `tests/test_sdbus_auth.c`
- Modify: `Makefile` (`test:`)

**Interfaces:**
- Produces (conn):
  ```c
  typedef struct {
      int fd; int id;
      int authed, said_hello, negotiated_fd;
      uid_t uid; gid_t gid; pid_t pid; int gids[64]; int n_gids;
      const char *unique;                /* ":1.N", set at Hello */
      unsigned char *in; int in_len, in_cap;
      unsigned char *out; int out_len, out_cap;
      int pending_fds[16]; int n_pending_fds;   /* received, awaiting a full msg */
      sdbus_matchset *matches;
  } sdbus_conn;
  void sdbus_conn_capture_creds(sdbus_conn *);   /* SO_PEERCRED + getgrouplist on fd */
  ```
- Produces (auth):
  ```c
  /* feed newly-read auth-phase bytes; append any response to conn->out.
     returns 1 when BEGIN reached (authed), 0 mid-handshake, -1 to reject/close. */
  int sdbus_auth_feed(sdbus_conn *, const unsigned char *buf, int len);
  ```

- [ ] **Step 1: Write the failing auth test** — drive the line protocol against a fake conn (no real socket; set `uid` directly): feed `"\0AUTH EXTERNAL 31303030\r\n"` (hex of "1000"), assert response contains `OK`; feed `"NEGOTIATE_UNIX_FD\r\n"`, assert `AGREE_UNIX_FD` and `negotiated_fd==1`; feed `"BEGIN\r\n"`, assert return 1 and `authed==1`. Feed a bogus mechanism → assert `REJECTED` and return 0 (stays unauthed) or -1 per your chosen policy (document which). Test `sdbus_conn_capture_creds` separately via a `socketpair` (both ends same process → uid is self; assert `uid==getuid()` and `n_gids>0`).

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sdbus_conn.h` + `sdbus_auth.h`** — `capture_creds`: `getsockopt(SO_PEERCRED)` → `struct ucred`; `getgrouplist(pwname_of_uid, gid, ...)` into `gids`. Auth state machine: consume the leading NUL byte, then CRLF-delimited commands; support `AUTH EXTERNAL <hex-uid>` (verify the decoded uid equals `conn->uid` from SO_PEERCRED — mismatch → REJECTED), `NEGOTIATE_UNIX_FD` → `AGREE_UNIX_FD`, `BEGIN` → authed, `CANCEL`/unknown → `REJECTED`. Append responses to `conn->out`.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Wire into `test:`, commit.**
```bash
git add sdbus_conn.h sdbus_auth.h tests/test_sdbus_auth.c Makefile
git commit -m "feat(schema-dbus): connection creds + SASL EXTERNAL auth"
```

---

## Task 8: Driver — `org.freedesktop.DBus` method dispatch

**Files:**
- Create: `sdbus_driver.h`
- Modify: `Makefile` (`schema-dbus` dep already lists it)

**Interfaces:**
- Consumes: `sdbus_names`, `sdbus_conn`, `sdbus_codec`, `sdbus_replies`, `sdbus_match`.
- Produces:
  ```c
  /* Given a method_call addressed to org.freedesktop.DBus from conn c, perform
     the driver action (mutating names/matches), and marshal a method_return (or
     error) into c->out. Emits any NameOwnerChanged/NameAcquired/NameLost via the
     provided broadcast callback. Returns 0 on handled, -1 if unknown member. */
  int sdbus_driver_dispatch(sdbus_msg *call, sdbus_conn *c, sdbus_names *names,
      void (*broadcast)(void *ctx, sdbus_transition *t, int n), void *ctx);
  ```

- [ ] **Step 1: Write the failing test** (`tests/test_sdbus_driver.c`) — construct a `Hello` call → assert reply body is the conn's unique name and `said_hello` set; a `RequestName("org.x", DO_NOT_QUEUE)` when free → reply code PRIMARY_OWNER and a NameOwnerChanged transition passed to broadcast; `GetNameOwner("org.x")` → reply is the owner unique; `NameHasOwner("org.missing")` → reply false; `ListNames` includes `org.freedesktop.DBus` and any owned names; `AddMatch` returns empty method_return and installs the rule on `c->matches`; `StartServiceByName` → error `org.freedesktop.DBus.Error.ServiceUnknown`.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sdbus_driver.h`** — dispatch on `member`. Use libdbus to build replies (`dbus_message_new_method_return`, append args with `dbus_message_iter_*`) then marshal into `c->out` via the codec. Map each method to the registry/match/conn calls. `GetConnectionUnixUser`/`ProcessID`/`Credentials` read from the target connection's cached creds (look up by the argument name → owner conn). Route driver-emitted transitions through `broadcast`.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Wire test into `test:`, commit.**
```bash
git add sdbus_driver.h tests/test_sdbus_driver.c Makefile
git commit -m "feat(schema-dbus): org.freedesktop.DBus driver methods"
```

---

## Task 9: Router + event loop + fd passing (`schema-dbus.c`)

This is the integration task: the epoll loop, `recvmsg`/`sendmsg` with `SCM_RIGHTS`, and `sdbus_route.h`'s `deliver()`. No new pure-logic unit test framework fits an event loop cleanly; correctness is proven by the live interop test (Task 10). Keep `deliver()` in `sdbus_route.h` so its decision logic *is* unit-testable with fake conns.

**Files:**
- Create: `sdbus_route.h`
- Modify: `schema-dbus.c` (replace the stub `main`)

**Interfaces:**
- Produces:
  ```c
  /* Decide delivery targets for msg from sender conn. Fills targets[] with
     conn_ids and returns count; sets *synth_error non-zero if the caller should
     instead return ServiceUnknown to the sender. Applies the policy gate:
     returns -1 (and *denied=1) if policy denies the send. */
  int sdbus_route_targets(sdbus_msg *msg, sdbus_conn *sender, sdbus_names *names,
      sdbus_conn **all, int n_all, sdbus_policy *pol, sdbus_replies *replies,
      int *synth_error, int *denied, int *targets, int max_targets);
  ```

- [ ] **Step 1: Write a failing routing-decision unit test** (`tests/test_sdbus_route.c`, fake conns, no sockets) — a method_call to a well-known name owned by conn 2 returns targets=[2]; to an unowned name sets `*synth_error`; a signal with no destination returns all conns whose matchset accepts it; a signal WITH a destination returns just that owner; a method_return whose reply_serial matches a recorded pending entry returns the caller and bypasses policy; a `send_destination`-denied call sets `*denied`.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sdbus_route.h`** — build an `sdbus_req` from the `sdbus_msg`+sender creds (op="send"), fill `dest_names` from `sdbus_names` ownership of the destination; if it's a reply (`has_reply_serial` + return/error) skip the policy gate (matches `_is_requested_reply`) and resolve the target via `sdbus_replies_match`; else `sdbus_policy_eval` → deny sets `*denied`; then resolve unicast/broadcast/directed-signal targets.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Implement the event loop in `schema-dbus.c`** — concrete structure:
  - Create the listen socket: `socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0)`, bind to `/run/dbus/system_bus_socket` (unlink stale first), `listen()`, `chmod 0777` per system bus convention.
  - `epoll_create1`, add the listen fd. Loop `epoll_wait`.
  - On listen readable: `accept4(...SOCK_NONBLOCK)`, allocate `sdbus_conn`, `sdbus_conn_capture_creds`, add to conn table + epoll.
  - On client readable: `recvmsg` with a `cmsg` buffer sized for `SCM_RIGHTS`; append payload to `conn->in`, stash any received fds in `conn->pending_fds`. If `!authed`, feed `sdbus_auth_feed`; else loop `sdbus_codec_take` over `conn->in`. For each full message: if not `said_hello` require Hello; if destination is `org.freedesktop.DBus` → `sdbus_driver_dispatch`; else `sdbus_route_targets` → on `denied` synth an `AccessDenied` error to sender, on `synth_error` synth `ServiceUnknown`, else `sdbus_codec_emit` (stamping sender) into each target's `out`, attaching the message's unix fds to the FIRST target's pending-out fd list.
  - Enable `EPOLLOUT` for a conn whenever its `out` is non-empty; on writable, `sendmsg` flushing `out` plus any pending-out fds via `SCM_RIGHTS`; disconnect if `out` exceeds the backpressure cap.
  - On client hangup/error: `sdbus_names_disconnect` (broadcast transitions), `sdbus_replies_purge`, `sdbus_match_free`, close fd, free conn.

- [ ] **Step 6: Build the whole daemon**

Run: `make schema-dbus`
Expected: links clean, no warnings.

- [ ] **Step 7: Wire `test:` for the route test, commit.**
```bash
git add sdbus_route.h schema-dbus.c tests/test_sdbus_route.c Makefile
git commit -m "feat(schema-dbus): routing decisions, epoll loop, SCM_RIGHTS fd passing"
```

---

## Task 10: Live interop test on a scratch socket

**Files:**
- Create: `tests/sdbus_live_interop.sh`
- Modify: `Makefile` (`verify-live:` target)

- [ ] **Step 1: Write the failing interop script** — `tests/sdbus_live_interop.sh`:
  - Start `./schema-dbus --system` with `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/sdbus-test.sock` (add an env/arg override so it binds a scratch path, not the real system socket — implement the override in `schema-dbus.c` Step where the bind path is chosen: honor `$SCHEMA_DBUS_SOCKET` if set).
  - `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/sdbus-test.sock busctl --system list` → expect it to include `org.freedesktop.DBus`.
  - `busctl --system call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus GetId` → expect a non-empty id.
  - Register a name with `dbus-send`/`busctl` from a second client, then `GetNameOwner` from a first → expect the owner unique.
  - Subscribe to `NameOwnerChanged`, register a name, expect the signal.
  - An fd-passing round-trip: use `busctl` against a tiny helper that returns a unix fd (or `systemd-run`-free: a small python `dbus` client that calls a method returning an fd) → expect the fd received.
  - Exit non-zero on any failure.

- [ ] **Step 2: Run to verify it fails** (daemon lacks the socket override / a method).

- [ ] **Step 3: Implement the `$SCHEMA_DBUS_SOCKET` override** in `schema-dbus.c` and fix whatever the script surfaces.

- [ ] **Step 4: Run to verify it passes**

Run: `bash tests/sdbus_live_interop.sh`
Expected: prints per-check OK lines, exits 0.

- [ ] **Step 5: Add a `verify-live:` Makefile entry that runs it; commit.**
```bash
git add tests/sdbus_live_interop.sh schema-dbus.c Makefile
git commit -m "test(schema-dbus): live interop on scratch socket incl fd passing"
```

---

## Task 11: Point the real shims at it

**Files:** none (validation task); may create `tests/sdbus_shim_check.sh`.

- [ ] **Step 1: Start `schema-dbus` on the scratch socket.** Launch `schema-logind.py` and `schema-systemd1.py` with `DBUS_SYSTEM_BUS_ADDRESS` pointed at the scratch socket (they connect as clients).

- [ ] **Step 2: Assert they own their well-known names** — `busctl --system --address=... list` shows `org.freedesktop.login1` and `org.freedesktop.systemd1`. Assert a representative call to each answers (`busctl introspect org.freedesktop.login1 /org/freedesktop/login1`).

- [ ] **Step 3: Capture + re-gate** — run `schema_dbus_learn.py` against the scratch socket for a short provoked session, then `verify_dbus_policy_live.py` on that capture → expect `false_positives=0 false_negatives=0` on the SP1-brokered bus.

- [ ] **Step 4: Commit the check script (if created).**
```bash
git add tests/sdbus_shim_check.sh
git commit -m "test(schema-dbus): logind/systemd1 shims register on the C bus"
```

---

## Task 12: Activatable-service inventory (deferral safety net)

**Files:**
- Create: `scripts/sdbus-activatable-audit.sh`, `docs/superpowers/notes/2026-09-02-activatable-inventory.md`

- [ ] **Step 1: Write the audit script** — enumerate `/usr/share/dbus-1/system-services/*.service`, extract each `Name=`, and check whether it is owned by a service on the schema-init rail (grep `services/*.svc`). Emit three buckets: (a) railed, (b) not railed (flag for human triage), (c) all names + their `Exec=`.

- [ ] **Step 2: Run it, write the inventory doc** — record the three buckets. For bucket (b), mark each boot/desktop-critical or not (human judgement; `nm_priv_helper`, `intel_lpmd` seen live). Any marked critical → add a rail-pin `.svc` in this task and note it. Non-critical → documented as "dark until v1.1."

- [ ] **Step 3: Commit.**
```bash
git add scripts/sdbus-activatable-audit.sh docs/superpowers/notes/2026-09-02-activatable-inventory.md services/
git commit -m "chore(schema-dbus): activatable-service inventory + any critical rail-pins"
```

---

## Task 13: Cutover wiring + vmtest + doctor check

**Files:**
- Modify: `services/dbus.svc`, `Makefile` (`install:`), `schema-doctor` (or its checks file)

- [ ] **Step 1: Add the doctor check** — extend schema-doctor with a `dbus-bus` check: `/run/dbus/system_bus_socket` exists AND a `Ping` to `org.freedesktop.DBus` returns. Add its test alongside the existing `tests/test_doctor_*.py`. Run that test → PASS.

- [ ] **Step 2: Make `install:` deploy the binary** — install `schema-dbus` to `/usr/local/bin/schema-dbus` (or `$(PREFIX)/bin`). Do NOT yet change `dbus.svc` in `install:`.

- [ ] **Step 3: Prepare the cutover form of `dbus.svc`** — a copy `services/dbus.svc.sp1` with `exec=/usr/local/bin/schema-dbus`, same `args=--system`, same `ready_path`. Keep the stock `exec=/usr/bin/dbus-daemon` line as a commented fallback in that file. Do not overwrite the live `dbus.svc` yet.

- [ ] **Step 4: vmtest** — run the `schema-vmtest` skill with `dbus.svc` swapped to the SP1 form. Assert: boots to graphical, `schema-logind`/`schema-systemd1` own their names, a fresh live-gate capture on the VM reads 0/0. Record the result.

- [ ] **Step 5: Commit the cutover artifacts (still inert on hardware).**
```bash
git add services/dbus.svc.sp1 Makefile schema-doctor tests/test_doctor_dbus.py
git commit -m "feat(schema-dbus): cutover artifacts, doctor bus check, vmtest green"
```

- [ ] **Step 6: Hardware flip (Jonathan-gated, reboot-only)** — only after vmtest green and Jonathan's go: replace `dbus.svc` with the SP1 form on blakbox, reboot, confirm doctor `dbus-bus` green, then run a real organic live-gate capture → expect 0/0. This step is performed live with Jonathan present, not by an unattended executor.

---

## Self-Review

**Spec coverage:** §1 decisions → Global Constraints + Tasks 1/2/7/9. §2 modules → Tasks 2–9 (one task per module, driver+router folded appropriately). §3 names/routing/reply/sender-stamp/fd-passing → Tasks 3(stamp),4(names),6(reply),9(route+fd). §4 policy port + conformance oracle → Task 2. §5 auth/lifecycle → Task 7 + Task 9 loop. §6 cutover → Task 13. §7 activatable inventory → Task 12. §8 test tiers → Tasks 2(conformance),10(interop),11(shims),13(vmtest+hardware). §9 v1.0/v1.1 line → Global Constraints (activation deferred) + Task 8 stub. §10 risks → each mitigated by its task's test. No spec section is unmapped.

**Placeholder scan:** no "TBD"/"implement later" steps; deferred items (activation) are an explicit non-goal, not a placeholder. Integration steps (Task 9 Step 5) give concrete syscalls/sequence rather than prose.

**Type consistency:** `sdbus_req`, `sdbus_msg`, `sdbus_conn`, `sdbus_names`, `sdbus_transition`, `sdbus_replies`, `sdbus_matchset` names are used identically across tasks; `sdbus_policy_eval` returns `"allow"`/`"deny"` consistently; RequestName flag/return enums defined once in Task 4 and consumed in Task 8.

**Known judgement calls left to the executor (not placeholders):** the exact backpressure cap (Task 9), whether a bad auth mechanism returns 0 vs -1 (Task 7, documented in-test), and the fd-passing interop helper choice (Task 10). Each is local and testable.
