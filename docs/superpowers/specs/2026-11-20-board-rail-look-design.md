# schema-board in the rail look — design (short)

**Intent.** The board on tty8 should look like the boot rail: schema font, neon chips, `S C H E M A//` banner, track glyphs. Jonathan approved the mockup 2026-11-20; "especially if everything fits on one page".

**Constraint found.** vconsole.conf now loads `schema-12x24` on every VT → tty8 is 160×45 at 1080p. 68 services in one column overflow (they already did at 8×16). Two columns fit (~2×40 slots).

**Approaches.**
- A. Restyle in place, two columns when they fit, one otherwise — **picked**: one file, no new deps, matches the rail's vocabulary.
- B. Share the rail's formatter (rail.c) — rejected: rail lines are events, board rows are states; coupling the two for a few SGR strings costs more than it saves.
- C. Shrink to fit (paging/scroll) — rejected: "fits on one page" is the point.

**Design.**
- Banner: `▓▒░ S C H E M A//B O A R D ░▒▓  host · SYSTEM · N services · seq S` (rail colours: 35/95/36/92).
- Recorder arrow line under the banner, prefixed `┃`.
- Rows: `┣━ name [STATE] wt pid rst cards [limp note]`; STATE as a reverse chip (`30;4x`, same colour map as today: purple PERFECT, green FUNDAMENTAL/SETTLED/FULL_TRUST, dim green NEW, yellow FRICTION/RECOVERY, blue DORMANT, red EXCISED; STANDBY dim white). Numbers dim grey (90). Limp notes stay yellow. Keeps board state names (cards key off them).
- Layout: column width fixed (~78); two columns side by side when `cols ≥ 2·width` and one column would overflow `rows`; column-major order.
- Groups: listed after services in the same flow.
- Footer: `━━►` + counts per state as chips, then the key help (interactive).
- Selection: bright `►` replaces `┣━` on the selected row (reverse row would clash with chips). Confirm prompt unchanged.
- Glyphs: font alphabet when the VT's font is 12×24 (KDFONTOP GET); ASCII alphabet otherwise. `--once` / non-VT stdout: today's plain output unchanged (vmtest stability).

**Non-goals.** No change to PID 1, shm, cards, or the rail.

**Left to the builder.** Exact column widths, name truncation (`…`), how counts collapse states.
