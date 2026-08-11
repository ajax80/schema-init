# schema-udev rule interpreter — R3 executor — design

## Context

Third slice of the udev rule interpreter (sub-project B-phase-2, parent design
`2026-08-10-schema-udev-rule-interpreter-design.md`). R1 (parser/loader) and R2
(matcher) are landed and verified on branch `feat/schema-udev-cutover-e3`
(commits `fb1ce77`..`7467d77`): `udev_ruleset.h` loads all 168 installed
`.rules` files into a `struct ruleset` and `rule_match(rule, dev_ctx)` decides,
per device, whether a single rule's *match* clauses are satisfied — including the
grouped parent-walk (`SUBSYSTEMS`/`KERNELS`/`DRIVERS`/`ATTRS`/`TAGS`).

What is still missing — and what actually blocks the E3 flip — is **applying**
the matched rules: a device's real `/run/udev/data` record carries `G:`/`Q:`
tags and `S:` symlinks produced by executing `TAG+=`/`SYMLINK+=` across the
ruleset in order, with `GOTO`/`LABEL` control flow. R3 is that executor.

R3 stays **shadow-only**. udevd (pid 207) remains authoritative; schema-udev
computes and is compared. No live `/dev` or `/run/udev/data` write is introduced.

### Grounding — assignment usage on this box (installed rules, 2026-08-11)

```
2586 ENV=      491 GOTO=     477 RUN+=(R4)   254 TAG+=    191 LABEL=
170 IMPORT=(R4) 152 MODE=    141 SYMLINK+=    83 GROUP=    47 ENV+=
23 OPTIONS+=    11 OWNER=      5 TAG=          5 NAME=      3 OPTIONS=
2 OPTIONS:=      1 NAME:=
```

`:=` finality: **3 total uses** (`OPTIONS:=` ×2 on `static_node`, `NAME:=` ×1),
none on tags/symlinks. OPTIONS sub-tokens seen: `static_node`, `link_priority`,
`string_escape`, `watch`, plus static-node values (`snd`/`vhost`/`vfio`/…). Of
these only `link_priority` (→ `L:`) and `string_escape` affect a compared record
field; `static_node`/`watch`/`db_persist` do not appear in `/run/udev/data`.

## Goals

- A driver `ruleset_apply(rs, ctx)` that walks the loaded ruleset in order and,
  for every rule whose match clauses pass (`rule_match==1`), applies its
  assignment clauses — accumulating into `struct dev_ctx` the device's **tag
  set, symlink set, node perms (mode/group/owner), name, and link priority**.
- Correct `GOTO`/`LABEL` forward control flow.
- ENV assignments mutate `ctx->ev` mid-run so a later rule's `ENV{K}==` sees the
  evolving state (this is why R2 made `rule_match` take a non-const `dev_ctx *`).
- Honest handling of R2's **superset** match set (see Decisions).

## Non-goals

- `IMPORT{}`, `RUN+=`, `TEST`, `PROGRAM`, `RESULT` and their deferred
  substitutions (`$result`/`$links`/`$name`/`$parent`) — R4.
- Writing records or `/dev` nodes / doing the actual link creation — R5 feeds the
  computed `dev_ctx` into `udev_db_record_build_full` and the by-* writer.
- The E3 flip. udevd stays authoritative.
- Modeling OPTIONS sub-tokens with no effect on the compared record on this box.

## Decisions settled during brainstorming

1. **Deferred gates → apply + flag (superset, reported not hidden).** R2's
   `rule_match` returns 1 for a rule whose *only* unsatisfied clauses are the
   R4-deferred conditionals (`TEST`/`PROGRAM`/`RESULT`). R3 applies on that
   verdict as-is; it does **not** skip such rules. To keep this honest,
   `rule_match` sets `ctx->last_rule_deferred` whenever it skipped a deferred
   conditional, and `ruleset_apply` bumps `ctx->deferred_applies` each time it
   applies a rule under that flag. R5's fidelity gate reads the counter and
   **reports** the potential over-application per device rather than silently
   masking it; R4 filling the gates is what actually resolves it. Rationale:
   matches the shadow-gate philosophy (we always know exactly what still
   diverges), avoids threading skip-state now, and R4 precedes R5 anyway so the
   gate is measured with the gates present.

2. **Minimal-for-fidelity scope.** Honor `TAG`, `SYMLINK`, `ENV`, `MODE`,
   `GROUP`, `OWNER`, `NAME`, and `OPTIONS{link_priority,string_escape}`. Treat
   `:=` as the corresponding assignment **plus** a per-`key{sub}` final-lock.
   Record-irrelevant OPTIONS (`static_node`, `watch`, `db_persist`, …) are parsed
   and ignored as tracked no-ops (must not error). YAGNI full OPTIONS/precedence
   modeling — nothing on this box needs it and R5 will surface it if that ever
   changes.

3. **Control flow — label map built at apply start.** A `GOTO="x"` jumps to the
   rule carrying `LABEL="x"`. Resolve via a label→rule-index map computed once
   per `ruleset_apply` (linear scan of `rs`). Optimize to a load-time index only
   if measured to matter — the parent-walk, not this, is the flagged cost.

## Architecture

All in `udev_ruleset.h`, extending the existing R1/R2 structures. New tests in
`tests/test_udev_executor.c`, wired into the `make test` target.

### `struct dev_ctx` additions

```c
#define DEVCTX_SYMLINKS_MAX 32
#define DEVCTX_FINAL_MAX    16

struct dev_ctx {
    /* ...existing: ev, sysroot, sysdir, tags/ntags, matched_parent... */
    char symlinks[DEVCTX_SYMLINKS_MAX][UE_VAL_MAX];
    int  nsym;
    char mode[8];                 /* "" = unset */
    char group[UE_KEY_MAX];       /* "" = unset */
    char owner[UE_KEY_MAX];       /* "" = unset */
    char name[UE_VAL_MAX];        /* "" = unset (record-only in shadow) */
    int  link_priority;           /* 0 = default */
    int  escape;                  /* 0 = none, 1 = replace */
    char final_keys[DEVCTX_FINAL_MAX][RK_KEY_MAX + RK_SUB_MAX + 2]; /* "KEY{sub}" */
    int  nfinal;
    int  last_rule_deferred;      /* set by rule_match per rule */
    int  deferred_applies;        /* bumped by ruleset_apply */
};
```

`dev_ctx_init` zeroes these (already `memset`s the struct).

### Helpers

- `ctx_add_tag` / `ctx_del_tag` / `ctx_clear_tags` — dedupe on add, bounded by
  `DEVCTX_TAGS_MAX`.
- `ctx_add_symlink` / `ctx_del_symlink` / `ctx_clear_symlinks` — a `SYMLINK`
  value may hold several space-separated links; the caller splits, each goes
  through substitution + `escape` before add. Dedupe, bounded.
- `ctx_key_final(ctx, clause)` / `ctx_lock_final(ctx, clause)` — build the
  `"KEY{sub}"` token; check/append the final-lock set.
- `apply_options(ctx, value)` — split on `,`/space; `link_priority=N` →
  `atoi` into `link_priority`; `string_escape=replace` → `escape=1`,
  `string_escape=none` → `escape=0`; anything else ignored (no-op).
- `label_index_build(rs, out_names, out_idx, max)` → count; linear scan
  collecting each rule's `LABEL=` value and its rule index.

### `apply_rule` (one matched rule)

Iterate the rule's clauses in order. Skip match-op clauses (already evaluated).
For each assignment clause, dispatch on `key`:

| key | `+=` | `-=` | `=` / `:=` |
|-----|------|------|-----------|
| `ENV{K}` | set prop (udev: += ≡ set) | (n/a) | set prop; `:=` locks |
| `TAG` | add-dedupe | remove | clear-then-add; `:=` locks |
| `SYMLINK` | split+add each | remove | clear-then-add; `:=` locks |
| `OPTIONS` | apply_options | — | apply_options; `:=` locks |
| `MODE`/`GROUP`/`OWNER`/`NAME` | (rare) set | — | set scalar; `:=` locks |
| `GOTO` | — | — | signal jump to `val` |
| `LABEL` | — | — | no-op marker |

Before any set/add, if `ctx_key_final` for that `key{sub}` → skip the clause.
Values (except `GOTO`/`LABEL` targets) pass through `ruleset_subst`; R4 tokens
copy verbatim. Return a jump signal (target label or "none").

### `ruleset_apply(rs, ctx)` (the driver)

```
build label map
for (i = 0; i < rs->n; ) {
    if (!rule_match(&rs->rules[i], ctx)) { i++; continue; }
    if (ctx->last_rule_deferred) ctx->deferred_applies++;
    jump = apply_rule(&rs->rules[i], ctx);
    if (jump target) i = label_index[target];   /* not found: i = n (stop) */
    else i++;
}
return 0;
```

`rule_match` resets `ctx->last_rule_deferred=0` at entry and sets it to 1 when it
skips an unknown match key (the TEST/PROGRAM/RESULT path already at line ~345).

## Testing strategy (TDD, `tests/test_udev_executor.c`)

Each behavior is a rule-fixture (inline `.rules` text → parse → apply → assert on
`dev_ctx`), matching the R1/R2 harness style:

1. `ENV{FOO}="bar"` sets the property; a later rule's `ENV{FOO}=="bar"` matches
   and fires its `TAG+=` — proves `ev` grows mid-run.
2. `TAG+=` accumulates and dedupes; `TAG-=` removes; `TAG="only"` clears then
   sets a single tag.
3. `SYMLINK+="a b"` yields two links; `SYMLINK-=` removes; `SYMLINK="x"` clears
   then sets; `string_escape=replace` transforms a link with a `/` or space.
4. `MODE`/`GROUP`/`OWNER`/`NAME` set their scalars; `NAME:=` then a later `NAME=`
   is blocked (final-lock).
5. `OPTIONS+="link_priority=10"` → `link_priority==10`;
   `OPTIONS+="string_escape=replace"` → `escape==1`;
   `OPTIONS+="static_node=foo"` is a no-op and does not error.
6. `GOTO="skip"` jumps past an intervening `TAG+=` (that tag must be absent) to
   the `LABEL="skip"` rule; a `GOTO` to a label past the last rule stops cleanly.
7. Deferred-gate: a rule `TEST=="/nonexistent", TAG+="x"` still applies `x`
   (superset) **and** leaves `deferred_applies > 0`.
8. Real-rules live smoke (like R2's): load the installed rule dirs, apply to a
   real device present on the box (e.g. a `hidraw`/`input` node), assert an
   expected tag lands (`uaccess` or `security-device` / `power-switch`
   depending on the device chosen), guarded to skip if the device is absent.

## Verification

- `make test` exit 0, **zero warnings** under both `-std=c11` and `-std=c99`.
- `schema-udev` daemon still builds clean (header-only change; it does not yet
  call `ruleset_apply`).
- Live box untouched: `mode=dry-run`, flip sentinel absent, deployed
  `/usr/bin/schema-udev` md5 unchanged, udevd pid 207 authoritative. R3 touches
  only `udev_ruleset.h` + the new test + Makefile; the built binary is gitignored
  and not installed.

## Risks

- **Superset over-application** until R4 — bounded and *reported* via
  `deferred_applies` (decision 1), resolved by R4 before the R5 gate is read.
- **`escape` fidelity** — `string_escape` semantics must match udev's
  `replace`/`none` for the S: comparison; covered by test 3, re-checked in R5
  against real records.
- **Ancestor-tag tracking still deferred** — `TAGS==` on a parent uses the
  device's own tag set (per R2 spec Risks); unchanged here, revisited in R5.

## Carry into R4 / R5

- R3's applied set follows R2's superset verdict; `deferred_applies` marks the
  devices where a `TEST`/`PROGRAM`/`RESULT` was skipped. R4 must re-gate those;
  R5 must not treat "R2 match" as "should apply."
- `NAME`/`MODE`/`GROUP`/`OWNER` are computed but not `/run/udev/data` fields —
  they feed the eventual live node apply, not the R5 record-fidelity comparison.
