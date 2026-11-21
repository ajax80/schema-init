# schema-board rail look Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** schema-board on tty8 drawn in the boot rail's look, all services on one screen.

**Architecture:** `render()` in `schema-board.c` gets a styled path (banner, chip rows, footer, 1–2 columns) chosen when stdout is a VT in `KD_TEXT`; the plain path stays byte-identical for `--once`/pipes. Glyph alphabet comes from `rail_glyphs.h` (font vs ASCII by console font height).

**Tech Stack:** C99, Linux console ioctls (KDGETMODE, KDFONTOP, TIOCGWINSZ), sh test + inline C helper.

**Spec:** `docs/superpowers/specs/2026-11-20-board-rail-look-design.md`

## Global Constraints

- Plain output (non-VT stdout, `--once` without override) unchanged byte-for-byte — `tests/test_recorder_daemon.sh` keeps passing.
- Colour map unchanged from `st_color()`; chips are `30;4x` of the same hue (dim green → `2;30;42`, STANDBY → `2;30;47`, unknown → `30;47`).
- Board keeps its own state names (`st_name()`).
- No change to PID 1, shm layout, cards, or rail.c.
- Test hooks: `SCHEMA_BOARD_STYLE=font|ascii|plain` forces the path; `SCHEMA_BOARD_SIZE=<cols>x<rows>` overrides TIOCGWINSZ.

## Review Focus

- More services than two columns hold (e.g. 120 at 160×45) → last visible slot reads `… N more`, never scrolls the banner off. Test in Task 2.
- Narrow console (<2 column widths) → one column, still truncates with `… N more`. Test in Task 2.
- Service name longer than the name field → cut with `…` (font) / `~` (ascii), row width stays fixed so the second column aligns. Test in Task 1.
- Name bytes with ESC/control/non-font codepoints → `?`, no escape injection on tty8. Test in Task 1.
- Interactive selection in the second column → `►` lands on the right row (column-major index). Test in Task 2.

---

### Task 1: Styled single-column render

**Files:**
- Modify: `schema-board.c` (render(), new helpers; include `rail_glyphs.h`)
- Create: `tests/test_board_look.sh` (builds fake shm via inline C like `tests/test_recorder_daemon.sh:16-41`; helper `mk NAME N` fills N services cycling every state, one long name, one name with `\033[2J`)
- Modify: `Makefile` `test:` target — add `sh tests/test_board_look.sh` after `test_recorder_daemon.sh`

**Interfaces:**
- Produces: `static int board_style(void)` → 0 plain, 1 ascii, 2 font (env override, else KDGETMODE==KD_TEXT on stdout, then KDFONTOP GET height==24 → font); `static const char *st_chip(uint8_t s)` → bg SGR; `static void put_row(const shm_svc_t *v, int selected, int width, const struct rail_alphabet *ab)` writes exactly `width` visible columns (no newline); `static void render_styled(const schema_shm_t *s, int style, int cols, int rows)`.

- [ ] **Step 1: Write the failing test** — `SCHEMA_BOARD_STYLE=font SCHEMA_BOARD_SIZE=80x60 ./schema-board --once` on 10 services; assert: output contains `S C H E M A//B O A R D`, `┣━`, `\033[30;45m` (PERFECT chip), `\033[30;41m` (EXCISED chip), `━━►`; long name ends in `…`; no raw `\033[2J` after the first line; `SCHEMA_BOARD_STYLE=ascii` output contains `+-` and `==>` and no byte ≥ 0x80 (`LC_ALL=C grep -q '[^ -~[:cntrl:]]'` finds nothing); unset STYLE → output identical to pre-change plain (compare against `SCHEMA_BOARD_STYLE=plain`).
- [ ] **Step 2: Run** `make schema-board && sh tests/test_board_look.sh` — Expected: FAIL (no banner).
- [ ] **Step 3: Implement** — banner `banner_l S C H E M A//B O A R D banner_r  host · SYSTEM · N services · seq S` with rail colours (35 / 95 / 36 / 92); arrow line prefixed `track_v`; rows `branch name chip wt pid rst cards note` (name field 22 cols via the same clean rule as rail.c `clean()` — copy, it is static there; numbers `\033[90m`); selected row uses `►` (font) / `>` (ascii) in place of `branch`; footer `finish` + per-state count chips + key help when interactive; confirm prompt reused from plain path.
- [ ] **Step 4: Run** `sh tests/test_board_look.sh && sh tests/test_recorder_daemon.sh` — Expected: both PASS.
- [ ] **Step 5: Commit** `board: rail look — banner, chips, track glyphs`

### Task 2: Two columns + overflow

**Files:**
- Modify: `schema-board.c` (render_styled layout, selection mapping)
- Modify: `tests/test_board_look.sh`

**Interfaces:**
- Consumes: `put_row`, `render_styled` from Task 1.
- Produces: `static int board_layout(int n, int cols, int rows, int width, int *ncol, int *per_col)` → visible slot count; column-major: item i at column `i / per_col`, row `i % per_col`.

- [ ] **Step 1: Write the failing test** — 68 services at `160x45`: every service name appears; output line count ≤ 45; a line holds two `┣━`. 120 services at `160x45`: `… ` + `more` appears, line count ≤ 45. 68 at `80x45`: one `┣━` per line, `more` appears. Interactive selection can't be driven by --once, so expose `SCHEMA_BOARD_SEL=<i>` (test-only, honoured with STYLE set) and assert at 160x45 with sel=50 the `►` line also contains service #50's name in its second half.
- [ ] **Step 2: Run** — Expected: FAIL.
- [ ] **Step 3: Implement** — row width 78; header rows = banner + arrow(if shown) + groups block + footer(+interactive lines); `per_col = rows - header`; `ncol = (cols >= 2*80 && n > per_col) ? 2 : 1`; when `n > ncol*per_col`, last slot shows `… N more` (dim). Groups follow services in the same flow.
- [ ] **Step 4: Run** `sh tests/test_board_look.sh` — Expected: PASS.
- [ ] **Step 5: Commit** `board: two columns so 68 services fit at 12x24`

### Task 3: Docs + hardware check

**Files:**
- Modify: `README.md:81` (one clause: drawn in the rail look on a VT), `docs/operations.md` recovery-console section (the env overrides, one line)

- [ ] **Step 1:** `make test` — Expected: all green.
- [ ] **Step 2:** install build on blakbox; restart board: `sudo schema-ctl restart schema-board` (not a session daemon — safe live); Jonathan checks ctrl-alt-F8: all services on one screen, chips match the rail.
- [ ] **Step 3: Commit** `docs(board): rail look`; push to `feat/rail-look` (#554).
