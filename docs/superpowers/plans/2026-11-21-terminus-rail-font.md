# Terminus 10×20 Rail Font Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Spleen-based `schema-12x24` rail font with `schema-10x20` built from upstream Terminus `ter-u20n`.

**Architecture:** Same pipeline as today: `tools/mkschemafont.py` cuts the source BDF to 256 glyphs and draws what's missing → `tools/bdf2psf.py` → `tools/psf2c.py` → `schema_font.c` embedded in PID 1. Only the source font, cell size and name change, plus the board's "is this our font" check.

**Tech Stack:** C (PID 1, schema-board), Python 3 font tools, Make, RPM spec, debian packaging.

**Spec:** `docs/superpowers/specs/2026-11-21-terminus-rail-font-design.md`

## Global Constraints

- Source: Terminus 4.49.1 `ter-u20n.bdf`, vendored unchanged as `fonts/ter-u20n.bdf`; licence OFL-1.1 (`fonts/OFL.terminus` = upstream `OFL.TXT`). Upstream tarball: `https://downloads.sourceforge.net/project/terminus-font/terminus-font-4.49/terminus-font-4.49.1.tar.gz`, sha256 `d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79`.
- Font name `schema-10x20`; cell 10×20; BDF `BBX 10 20 0 -4`; 256 glyphs (never 512 — bright colours).
- Hand-drawn glyphs: exactly ╍ U+254D, ◇ U+25C7, □ U+25A1. Everything else from Terminus.
- Embedded glyph pitch stays 64 bytes (32 rows × 2 bytes); `schema_font_glyphs[256 * 64]` unchanged.
- Historical docs under `docs/superpowers/` keep the old name.
- No compatibility handling for the old `schema-12x24` name (CLAUDE.md: no shims).

## Review Focus

1. **A user font of the same size (e.g. `setfont ter-v20n`, which Jonathan has used on tty3):** the board must fall back to ASCII glyphs, not print rail glyphs the font lacks. Today's check is size-only (12×24), so a 10×20 check would misfire on stock Terminus. → Task 3 changes the check to "font has U+254D in its map".
2. **blakbox `/etc/vconsole.conf` still says `FONT=schema-12x24` after upgrade:** PID 1 treats it as a foreign font and skips loading, and vconsole-setup can't find the file. → Task 4 rollout switches it in the same step; Task 2 test pins that the old name no longer counts as ours.
3. **Drift test on armhf/mawk runners:** regenerated files must be byte-identical everywhere. → Task 1 runs `tests/test_font_drift.sh`; `deb` workflow must be green before merge (PR checks don't run deb — wait for the push-run on a test branch or check the `deb` run before merging).
4. **Shutdown restore of the previous font:** GET still at 32×32×512 pitch; unchanged, covered by existing `font_fail_each_step`.
5. **Screens 1280–1919 px wide:** 10×20 at 1280×720 gives 128×36; the 1280 threshold stays. No change.

---

### Task 1: Generate `schema-10x20` from Terminus

**Files:**
- Create: `fonts/ter-u20n.bdf`, `fonts/OFL.terminus`, `fonts/schema-10x20.bdf`, `fonts/schema-10x20.psfu`
- Delete: `fonts/spleen-12x24.bdf`, `fonts/LICENSE.spleen`, `fonts/schema-12x24.bdf`, `fonts/schema-12x24.psfu`
- Modify: `tools/mkschemafont.py`, `tools/bdf2psf.py`, `tools/psf2c.py`, `Makefile:516-526`, `tests/test_font_drift.sh`, `schema_font.c` (regenerated), `rail_glyphs.h:7,19`
- Test: `tests/test_schema_font.c` (existing), `tests/test_font_drift.sh`

**Interfaces:**
- Produces: `schema_font_glyphs[256*64]`, `schema_font_unimap[]`, `schema_font_unimap_len`, `schema_font_has(uint32_t)` — same symbols, new data. Make target name `test-schema-font` unchanged; regen target writes `fonts/schema-10x20.{bdf,psfu}`.

- [ ] **Step 1: Vendor the source.** `git rm fonts/spleen-12x24.bdf fonts/LICENSE.spleen`; copy `ter-u20n.bdf` and `OFL.TXT` (as `fonts/OFL.terminus`) from the tarball.

- [ ] **Step 2: Point the drift test and Makefile at the new names** (`fonts/ter-u20n.bdf` → `fonts/schema-10x20.bdf` → `.psfu` → `schema_font.c`). Run `sh tests/test_font_drift.sh` — Expected: FAIL (tools still 12×24).

- [ ] **Step 3: Retarget the tools.**
  - `mkschemafont.py`: `W, H = 10, 20`; parse var/messages say Terminus; `DRAWN` = {0x254D, 0x25C7, 0x25A1} only (delete `square_filled`, `triangle_up`, `pointer_right`); header rewrite `FAMILY_NAME "Terminus"`→`"schema"`, `-xos4-Terminus-`→`-schema-schema-`; COMMENT `schema-10x20: Terminus 4.49.1 ter-u20n cut to 256 glyphs, rail glyphs added`; per-glyph `DWIDTH 10 0`, `BBX 10 20 0 -4`.
  - Drawn shapes, on Terminus 20n metrics (◆ U+25C6 centre col 4 row 9, spans rows 5–13; ■ U+25A0 cols 2–6 rows 6–13; ━ U+2501 rows 9–10):
    - ╍: rows 9–10, cols 1–3 and 6–8.
    - ◇: pixels with `3 <= |x-4| + |y-9| <= 4`.
    - □: border of the ■ box — rows 6 and 13 cols 2–6, cols 2 and 6 rows 6–13.
  - `bdf2psf.py`: `W, H = 10, 20`; BBX check `['10','20','0','-4']`. bpg becomes 40.
  - `psf2c.py`: accept `(256, 20, 10)`; pad comment "each 20-row glyph out to 32"; generated header names `fonts/schema-10x20.psfu`.
  - `rail_glyphs.h`: comments name `schema-10x20`.

- [ ] **Step 4: Regenerate** with the Makefile regen recipe; `git rm fonts/schema-12x24.{bdf,psfu}`.

- [ ] **Step 5: Verify.** `sh tests/test_font_drift.sh` and `make test-schema-font` — Expected: both pass, `test_schema_font: ok`. Eyeball the three drawn glyphs: print their rows from `fonts/schema-10x20.bdf` as `#`/`.` art next to Terminus ◆ ■ ━.

- [ ] **Step 6: Commit** `font: schema-10x20 from Terminus ter-u20n, replacing Spleen 12x24`.

### Task 2: PID 1 loads 10×20 and recognises the new name

**Files:**
- Modify: `rail_console.c:42,68`, `tests/test_rail_console.c:72` and its KDFONTOP mock

**Interfaces:**
- Consumes: `schema_font_glyphs` from Task 1.
- Produces: `rc_font_wanted(...)` returns 1 for vconsole font `"schema-10x20"`.

- [ ] **Step 1: Failing tests.** In `font_wanted_rules`: replace the 12x24 line with
  ```c
  assert(rc_font_wanted("", "", "schema-10x20", 1920) == 1);
  assert(rc_font_wanted("", "", "schema-12x24", 1920) == 0);
  ```
  In the mock, record width/height of the last `KD_FONT_OP_SET`; in `font_fail_each_step` after the first `RC_FONT_OK`: `assert(set_w == 10 && set_h == 20);`.
- [ ] **Step 2:** `make test` (or the rail-console target) — Expected: FAIL on those asserts.
- [ ] **Step 3:** `rail_console.c`: name check `"schema-10x20"`; `font_set(fd, 10, 20, 256, ...)`.
- [ ] **Step 4:** tests pass.
- [ ] **Step 5: Commit** `rail: load schema-10x20`.

### Task 3: Board picks rail glyphs only for our font

**Files:**
- Modify: `schema-board.c:357-366` (`board_style`), comment at `:587`

**Interfaces:**
- Produces: `static int font_is_schema(int fd)` — 1 when the VT's Unicode map contains U+254D (╍, a glyph only schema-10x20 carries among common console fonts).

- [ ] **Step 1: Implement** `font_is_schema` with `GIO_UNIMAP` into a static `struct unipair[8192]` buffer; `board_style` returns `STYLE_FONT` when `font_is_schema(STDOUT_FILENO)`, else `STYLE_ASCII`. Drop the KDFONTOP size probe. Comments name schema-10x20.
- [ ] **Step 2: Verify on a real VT** (`tests/livetest/vmtest-vt.sh` or a spare tty): `setfont ter-v20n` → board draws ASCII look; `setfont fonts/schema-10x20.psfu` → board redraws in the rail glyphs within one refresh. Existing board tests (`SCHEMA_BOARD_STYLE` overrides) still pass under `make test`.
- [ ] **Step 3: Commit** `board: detect schema font by its glyph map, not its size`.

### Task 4: Packaging, docs, rollout

**Files:**
- Modify: `Makefile:138-142,298`, `.gitignore:40`, `schema-init.spec:490-491` (License tag becomes `AGPL-3.0-or-later AND OFL-1.1`), `debian/rules:19`, `debian/schema-init.install:6-7`, `debian/copyright:24-27`, `README.md:60`, `docs/operations.md:44,100`

- [ ] **Step 1:** Rename every `schema-12x24` → `schema-10x20`; `LICENSE.spleen` → `OFL.terminus`; debian/copyright stanza `Files: fonts/ter-u20n.bdf fonts/schema-10x20.* schema_font.c`, Copyright Dimitar Toshkov Zhekov, License OFL-1.1. README: "built on [Terminus](https://terminus-font.sourceforge.net/) (OFL-1.1)". Verify: `git grep -n -i '12x24\|spleen' -- . ':!docs/superpowers'` — Expected: no output.
- [ ] **Step 2:** `make clean && make && make test && make install DESTDIR=$(mktemp -d)` — Expected: green, `schema-10x20.psfu.gz` under `lib/kbd/consolefonts`. `rpmbuild`-style check via the spec's `%files` (local mock or COPR test build) and `dpkg-buildpackage -b -us -uc` if available; otherwise rely on CI `deb`.
- [ ] **Step 3: Commit** `pkg: ship schema-10x20 and the Terminus licence`.
- [ ] **Step 4: PR** draft → `/code-review` → push fixes → confirm the `deb` workflow is green (push the branch once as `pi-*` or check its run) → ready → merge.
- [ ] **Step 5: Rollout on blakbox** after the dev COPR build: in one step `dnf upgrade schema-init`, `sed -i 's/^FONT=schema-12x24/FONT=schema-10x20/' /etc/vconsole.conf`, `dracut -f`; remove `/usr/bin/schema-board.pre-look`; reboot. Verify: rail draws in 10×20 at boot, `tty8` board uses rail glyphs, `setfont` on tty3 to `ter-v20n` shows the board in ASCII.
