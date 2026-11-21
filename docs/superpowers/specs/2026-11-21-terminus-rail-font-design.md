# Terminus 10×20 rail font — design

Approved by Jonathan 2026-11-21 (normal weight).

## Intent
The boot rail and schema-board use `schema-12x24` (Spleen 12×24). Jonathan finds 12×24
"a little bigger than I'd like"; he saw Terminus 10×20 (`ter-v20n`) on tty3 and approved
that size. Replace the base font so the rail look is one size step down and all-Terminus.

## Goals
- `schema-10x20`: 256-glyph console font built from upstream Terminus `ter-u20n.bdf`.
- Same glyph set and slot policy as today (Latin-1 at identity slots, rail + light box
  mandatory, EXTRA fills the rest); 256 so the VT keeps bright colours.
- Rename `schema-12x24` → `schema-10x20` everywhere; drop Spleen.

## Non-goals
- No palette, layout, or rail-state changes.
- No bold variant, no size option.

## Anti-goals
- A font that mixes styles visibly (hand-drawn glyphs that don't match Terminus weight).
- Losing bright colours (512-glyph font).
- Breaking serial/vmtest/rail.log byte-identity.

## Approaches
1. **Upstream Terminus BDF (picked).** 1356 glyphs; lacks only ╍ ◇ □. Vendor
   `fonts/ter-u20n.bdf` + OFL.
2. Convert Fedora's `ter-v20n.psf`. Lacks 8 rail glyphs and 33 EXTRA — slots can't fill
   without more drawing or junk glyphs. Rejected.
3. Keep Spleen, scale down. Spleen has no 10×20. Rejected.

## Design
- `fonts/ter-u20n.bdf` (Terminus 4.49.1, unchanged) replaces `fonts/spleen-12x24.bdf`.
- `tools/mkschemafont.py`: W,H = 10,20; DRAWN = ╍ (U+254D), ◇ (U+25C7), □ (U+25A1), drawn
  on Terminus 20n metrics (match its ◆ U+25C6 and ■ U+25A0 for centre/x-height, ━ U+2501
  for the dash weight). Drop drawings Terminus now supplies (■ ▲ ►).
- Generated `fonts/schema-10x20.bdf` and `schema_font.c` regenerated; drift test
  (`tests/test_font_drift.sh`) keeps them honest.
- C: `rail_console.c` font_set 10,20 + wanted-name; `schema-board.c` style check 10x20;
  `rail_glyphs.h` comments; KDFONTOP still reads at 32-row pitch (psf2c pads 20→32).
- Packaging: Makefile, `schema-init.spec`, `debian/rules`, `debian/schema-init.install`,
  `.gitignore`; `debian/copyright` swaps Spleen (BSD-2) for Terminus (OFL-1.1) and the
  spec License tag follows.
- Docs: README, docs/operations.md mentions.
- Tests: `tests/test_rail_console.c` font name/size expectations.

## Rollout
One PR (draft → /code-review → merge when all checks incl. deb are green). After the
dev COPR build: set blakbox `/etc/vconsole.conf` FONT=schema-10x20 in the same step as the
upgrade (old font file disappears), reboot, verify rail at boot and tty8 board font.

## Left to the builder
Exact pixel shapes of the three drawn glyphs; whether historical spec/plan docs keep the
old name (they should — they're history).
