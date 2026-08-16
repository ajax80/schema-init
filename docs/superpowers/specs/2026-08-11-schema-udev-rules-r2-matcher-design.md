# schema-udev Rule Interpreter — R2 Matcher — design

## Context

R2 is the second slice of the rule interpreter (parent design:
`2026-08-10-schema-udev-rule-interpreter-design.md`). R1 delivered the
parser/loader: the installed `.rules` set is now readable into an in-memory
`struct ruleset` (`udev_ruleset.h`). R2 adds the **matcher** — given a parsed
rule and a device, decide whether the rule's match clauses apply. It executes
nothing (assignments, `GOTO`/`LABEL` control flow, `TAG+=`, `SYMLINK+=` are
R3); it only answers "does this rule match this device?".

Faithful matching is the precondition for faithful tag/symlink reproduction:
if R2 over- or under-matches, every downstream tag set is wrong and the E3
flip would silently break the pico-fido `security-device` ACL, `snap_*`
cgroups, and the power button.

## Goals

- Evaluate udev match clauses against a device with **byte-faithful udev
  semantics**, including the grouped parent-walk that a naive matcher gets
  wrong.
- Provide the two shared primitives the interpreter needs: a udev glob matcher
  (`|` alternation over `fnmatch`) and a match-time substitution expander.
- Pure, unit-testable, shadow-only. No live `/dev` or `/run/udev/data` write.

## Non-goals

- Executing assignments, control flow, or `IMPORT`/`RUN`/`PROGRAM` (R3/R4).
- Full match+execute fidelity vs `udevadm test` — that gate is R5, since it
  requires the executor.
- Substitution tokens that need executor/program context: `$result`/`%c`,
  `$links`, `$name`, `$parent` — deferred to R3/R4 (see Substitution below).

## Decisions settled during brainstorming

1. **Substitution scope — match-resolvable subset now.** R2 builds only the
   substitution tokens resolvable at match time. Tokens needing later context
   are passed through verbatim and recorded as a tracked reclaim TODO, never
   silently dropped.
2. **File layout.** All R2 code extends `udev_ruleset.h` (`static inline`,
   header-only), consistent with R1 and the codebase style.
3. **Glob via `fnmatch` + alternation wrapper.** `fnmatch` already handles
   `* ? [...]`; udev's only addition is top-level `|` alternation, so R2 wraps
   `fnmatch` rather than reimplementing globbing.

## Architecture — four units in `udev_ruleset.h`

### 1. `udev_glob(const char *pat, const char *str)` → 1/0
The udev glob primitive. Split `pat` on top-level `|` (alternation); return 1
if any alternative satisfies `fnmatch(alt, str, 0)`. No `FNM_PATHNAME` — udev
globs treat `/` as an ordinary character. `[...]` classes are passed to
`fnmatch` intact (a `|` inside a bracket class is not an alternation split;
the splitter must respect `[...]`).

### 2. `ruleset_subst(const char *in, const struct dev_ctx *ctx, char *out, size_t sz)`
Expand the **match-resolvable** substitution tokens:

| Token(s) | Source |
|----------|--------|
| `%k` / `$kernel` | device kernel name (basename of DEVPATH) |
| `%n` / `$number` | trailing digits of kernel name |
| `%p` / `$devpath` | DEVPATH |
| `%b` / `$id` | `ctx->matched_parent` — kernel name of the ancestor that satisfied the most recent parent-match group; empty before any group has matched (udev's `$id` semantic) |
| `%M` / `$major`, `%m` / `$minor` | MAJOR / MINOR properties |
| `$env{KEY}` / `%E{KEY}` | `uevent_get(ctx->ev, KEY)` |
| `$attr{FILE}` / `%s{FILE}` | `pi_sysattr(ctx->sysdir, FILE, …)` |
| `$driver` | DRIVER property |
| `$sys` / `%S` | sysfs mount root (`/sys`) |
| `$root` / `%r` | dev root (`/dev`) |
| `%%`, `$$` | literal `%`, `$` |

**Deferred (verbatim passthrough + `RECLAIM:` note):** `$result`/`%c`,
`$links`, `$name`, `$parent`. These need executor/PROGRAM context that does not
exist in R2; copying them verbatim keeps the string intact for R3/R4 to expand.

### 3. `struct dev_ctx` — the match target
```c
struct dev_ctx {
    struct uevent *ev;      /* properties; MUTABLE (R3 grows it) */
    const char    *sysroot; /* e.g. "/sys" */
    char           sysdir[PATH_MAX]; /* absolute sysfs dir of the device */
    char           tags[DEVCTX_TAGS_MAX][UE_KEY_MAX]; /* accumulates in R3 */
    int            ntags;
    char           matched_parent[UE_KEY_MAX]; /* last parent-group match's kernel name; "" until set */
};
```
The matcher sets `matched_parent` to the basename of the ancestor that
satisfies each parent-match group, so `$id`/`%b` resolves correctly during and
after the walk. `ruleset_subst` reads it; it is `""` before any group matches.
R2 only **reads** `ev`, `sysdir`, and `tags`. `ev` and `tags` are mutable
because R3's executor grows them and later match clauses (`ENV==`, `TAG==`)
must see the evolving state — exactly as udev evaluates a rule file top to
bottom against accumulating device state. R2 tests seed `ev`/`tags` directly.

### 4. `rule_match(const struct rule *r, const struct dev_ctx *ctx)` → 1/0
Walk `r->clause[0..nclause)` in order. **Skip assignment ops** (`=`, `+=`,
`-=`, `:=`) — they are R3. For each **match op** (`==`, `!=`):

**Device-level keys** — evaluated against the device itself:
`ACTION`, `DEVPATH`, `SUBSYSTEM`, `KERNEL` (basename of DEVPATH), `DRIVER`,
`ENV{K}` (`uevent_get`), `ATTR{F}` (`pi_sysattr` on `ctx->sysdir`),
`TAG` (membership test in `ctx->tags`). Compared with `udev_glob(value, actual)`;
`!=` negates the result. A missing property/attr fails an `==` and satisfies a
`!=` (udev semantics: absent value never equals a pattern).

**Grouped parent-walk keys** — `SUBSYSTEMS`, `KERNELS`, `DRIVERS`, `ATTRS{}`,
`TAGS`: **consecutive** parent-match clauses form a group. Climb the sysfs
ancestry from the device upward (self included) via `pi_parent`; the group
matches iff **some single ancestor satisfies every clause in the group**. On
success the clause cursor advances past the whole group. If no ancestor
satisfies all group clauses, the rule fails immediately. A naive
"any-ancestor-matches-any-clause" is wrong and would over-match; the
single-ancestor-satisfies-all rule is the udev semantic and is the core
correctness requirement of R2.

Per-ancestor evaluation inside a group: `SUBSYSTEMS`==`pi_subsystem(anc)`,
`KERNELS`==`pi_base(anc)`, `DRIVERS`==`pi_driver(anc)`, `ATTRS{F}`==
`pi_sysattr(anc,F)`, `TAGS`==membership. On a successful group match the
ancestor's basename is stored in `ctx->matched_parent` (for `$id`/`%b`).

The device's own `DRIVER` and each ancestor's `DRIVERS` come from a **new
`pi_driver(dir)` helper** (added to `path_id.h`): `readlink` on `<dir>/driver`,
take the basename — mirroring the existing `pi_subsystem`, since `driver` is a
symlink, not a plain attr file. `TAGS` membership uses the device tag set;
ancestor-only tag tracking is not in R2 (see Risks).

## Fidelity boundary for R2

R2 is verified at the **clause/rule level**, not the full-record level:

- Unit tests assert `udev_glob`, `ruleset_subst`, device-level `rule_match`,
  and the grouped parent-walk against synthetic uevents and a synthetic sysfs
  tree.
- A live smoke asserts, on a few real devices, that `rule_match` agrees with a
  known-true and a known-false real rule (anti-hollow: at least one 1 and one 0).
- **Full match+execute fidelity vs `udevadm test` / `/run/udev/data` is R5.**
  R2 cannot produce a full record (no executor), so no full-record gate is
  claimed here. Not faking a greener gate than the slice earns.

## Testing strategy

- **Unit (TDD, `make test`):**
  - `udev_glob`: `sd*|vd*` matches `sda`/`vdb`, rejects `hda`; `[0-9]` classes;
    `?`; a `|` inside `[...]` is not split.
  - `ruleset_subst`: each resolvable token expands; deferred tokens
    (`$result`, `$links`, `$name`, `$parent`) pass through verbatim.
  - device-level `rule_match`: `ACTION`/`SUBSYSTEM`/`KERNEL`/`ENV`/`ATTR`/`TAG`
    with `==` and `!=`, including missing-value semantics.
  - grouped parent-walk: build a synthetic sysfs tree in a `mkdtemp` dir
    (device → parent → grandparent, each with `subsystem` symlink + attr
    files); assert a group matches only when one ancestor satisfies all its
    clauses, and fails when the clauses are only satisfiable across two
    different ancestors.
- **Live smoke:** load the real installed ruleset (R1 `ruleset_load_dirs`),
  build `dev_ctx` for a couple of real devpaths, assert `rule_match` true/false
  against chosen real rules.
- **`schema-vmtest`:** headers are not in the PID-1 boot rail, so vmtest is a
  regression check that the tree still builds/boots, not an R2-specific gate.

## Risks

- **Parent-walk grouping correctness** — the one place fidelity is easy to lose.
  Bounded by an explicit synthetic-tree test that fails a naive matcher.
- **Ancestor-tag matching** (`TAGS` on a parent) — R2 lacks ancestor tag
  tracking; handled as device-tag set for now, tracked for R5. No installed
  rule on blakbox is known to require ancestor-only tag matching; if one does,
  R5's fidelity gate surfaces it.
- **Tree-walk cost** at coldplug (471 devices × parent chains) — measured in
  R5; R2 keeps the walk allocation-free (fixed buffers, `pi_parent` string
  truncation).

## Endgame position

R2 green gives the interpreter a faithful matcher. R3 (executor: assignments +
`GOTO`/`LABEL` + tag/symlink accumulation) consumes it, R4 adds
`IMPORT`/`RUN`/`TEST`/`PROGRAM`, R5 integrates and runs the full-record
fidelity gate across all 471 devices — the precondition for the E3 flip.
Nothing about the live box changes in this slice.
