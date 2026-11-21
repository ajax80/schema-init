# The rail look — design

2026-11-20. Status: look, font and approach chosen by Jonathan in the brainstorm (mockups: rail C + B's chips; font Spleen 16×32 as our own schema font; approach C).

## Why

The boot rail is the first thing anyone sees of schema-init. Today it is a log line per event on the console:

    [2026-11-20 16:06:01] sshd                  spawn         state=NEW_PROCESS  wt=0  pid=1300

Accurate, but it reads like a log, not like an init system with its own identity. Jonathan wants the boot to look futuristic and catchy ("cyberpunkish"): systemd's green `[ OK ]` idea, pushed much further, in the same colour language as `schema-board` (purple, green, yellow, red), with a much cooler font.

**Guiding rule:** the rail must never claim a service is healthy, a boot is settled, or the console has been restored unless the underlying state supports it.

## Goals

- On a real screen (a VT), the rail is a **status display**: a banner, a track down the left with one short line per meaningful event, a solid colour chip naming the state, and a finish line with chip counts. Same look for runtime events and for shutdown.
- **Our own console font**, `schema-16x32`, built on Spleen 16×32 with every glyph the design uses drawn in, so the rail looks identical on every box.
- **The exact neon colours**: PID 1 loads its 16-colour palette into the VT at start.
- **Detail lives in `rail.log`**, unchanged, plain text.

## Non-goals

- Plymouth / graphical splash. Boxes booting `rhgb quiet` keep their splash; this is the text console.
- Changing what the rail *records*. `rail.log`, the recorder, and every existing consumer see exactly what they see today.
- Styling the serial console or a log-file stdout (vmtest, the Pi's boot.log). They keep today's lines byte for byte.
- A theme system. One look; the off-switch is the only knob.

## Anti-goals

- A prettier boot that can **hide or delay** a failure. A service that hangs, fails or is cut must be as visible as today, and red/yellow must win attention.
- Anything that can **break or slow a boot**. Font/palette loading is best-effort: any failure falls back silently to plain text, never blocks PID 1, never retries in a loop.
- Escape codes in `rail.log`, in the recorder, or on a serial line that a test greps.

## Approaches (Jonathan picked C)

- **A — PID 1 loads font + palette itself** (embedded in the binary, two ioctls at start). Same on every distro, no config edits. Kernel/initramfs lines before PID 1 stay in the old font.
- **B — standard plumbing**: install the font, set `FONT=` in vconsole.conf (initramfs loads it), palette via `vt.default_red/grn/blu=` on the cmdline. Font from the first userspace line, but edits config and boot entries per box, differs per distro (Pi has no dracut), fights a user's own font.
- **C — A, plus the font installed as a normal console font** (`/usr/lib/kbd/consolefonts/schema-16x32.psfu.gz`, Debian: `/usr/share/consolefonts/`), so `setfont schema-16x32` works by hand and `FONT=schema-16x32` in vconsole.conf opts into the initramfs. **Chosen**: A's "just works" plus an opt-in for earlier.

## Design

### 1. When the styled rail is on

PID 1 decides once at start (and again after a re-exec, without reprinting the banner):

- stdout is a VT in text mode: `ioctl(1, KDGETMODE)` succeeds **and** returns `KD_TEXT` (a serial console or a file fails it; Plymouth holding the VT in `KD_GRAPHICS` keeps its splash, plain lines underneath), **and**
- not switched off: kernel cmdline `schema.rail=plain`, or the word `plain` in `/etc/schema-init/rail` (one-word config files, like `/etc/schema-init/hardening-default` and `/etc/schema-init/watchdog`; there is no schema-init.conf).

Otherwise every line is exactly today's line. This one rule keeps vmtest (console=ttyS0), the Pi wrapper's boot.log, and any `script`-captured console unchanged.

`rail.log` always gets today's line, styled mode or not (service_log already writes both; it keeps writing the plain line to the file).

The decision is per sink, not global: the shutdown path writes through its own non-blocking `/dev/console` fd (shut_write) and makes the same `KDGETMODE == KD_TEXT` test on that fd. With `console=tty0 console=ttyS0`, `/dev/console` is the last `console=` (serial), so shutdown stays plain there — accepted. The styled shutdown path keeps shut_write's non-blocking, drop-on-EAGAIN behaviour (the 07-26 console wedge); the boot rail keeps today's blocking stdout write, no new blocking introduced.

**Re-exec** (decision): after a live re-exec PID 1 does not try to recover boot-phase state: banner and finish line are never printed by a re-exec'd PID 1, even if the re-exec happened before settle; every later line uses the runtime shape (dashed track, wall clock). It re-runs the mode test and reapplies palette + font **without saving the console state again** (see §2a). No change to `reexec_state.h`.

### 2. Font and palette (best-effort, VT only)

At start, styled mode on:
- **Font**: `KDFONTOP`/`KD_FONT_OP_SET` with the embedded `schema-16x32` glyphs (**256** glyphs × 64 bytes = 16 KiB — a 512-glyph font makes the VT give up attribute bit 3 for the glyph index, leaving 8 foreground colours, and the design needs the bright ones) and `PIO_UNIMAPCLR` + `PIO_UNIMAP` with its Unicode table. These are separate ioctls, so loading is transactional: first save the current font (`KD_FONT_OP_GET`) and Unicode map (`GIO_UNIMAP`); if any later step fails, put both back. If putting them back also fails, rail.log gets a clear `console font restore FAILED` line and styled mode stays off — never a silent half-changed console. If the framebuffer is too small for 16×32 to give ≥ 80 columns, skip the font (keep the kernel's) but keep the styling. Measured once at start with `FBIOGET_VSCREENINFO` on `/dev/fb0` (`xres >= 1280` — a heuristic on the framebuffer mode at that moment, not a promise about the final console geometry); no `/dev/fb0` yet → skip the font, never wait. A later KMS takeover (simpledrm → real driver) keeps the loaded font (fbcon re-applies it per console); a smaller final mode is accepted. **User font policy** (decision: the configured preference, not detection of what's loaded — the kernel can't name the loaded font): if `/etc/vconsole.conf` (Debian: `/etc/default/console-setup` `FONTFACE=`) names a font other than empty/`eurlatgr`/`schema-16x32`, skip our font and keep the styling. The word `nofont` in `/etc/schema-init/rail` (or `schema.rail_font=off` on the cmdline) skips it unconditionally.
- **Palette**: `PIO_CMAP` with the 16 schema RGB values (below). In the kernel (`con_set_cmap`, drivers/tty/vt/vt.c) this is global: it rewrites the default palette and every allocated VT — the builder re-confirms on the 6.x/7.x kernels we ship. Accepted: the desktop VT draws its own pixels. A PID 1 crash leaves the schema palette until reboot — accepted.
- Any ioctl failure: plain text for the rest of the boot, one note line in rail.log, no retry.

### 2a. Console state across re-exec and shutdown

The pre-schema console state must survive a live re-exec (process memory doesn't), or a re-exec'd PID 1 would save the schema palette as "original".
- On the first apply, PID 1 writes the saved palette (48 bytes) to `/run/schema-init/console-orig` (0600, atomic). `/run` is tmpfs: it survives re-exec, not reboot — exactly the lifetime needed.
- On start, if that file exists and is valid, PID 1 uses it as the original and does not call `GIO_CMAP` to save again. Missing/invalid → save as on a first boot.
- Palette apply failing after a successful save: the file is not written (nothing to restore).
- **Shutdown restore is independent of shutdown styling**: just before the final reboot/poweroff, if the file exists, PID 1 opens a VT directly (`/dev/tty0` — any VT works since the palette is global) and `PIO_CMAP`s the saved palette, whatever `/dev/console` is (serial or not).
- **Font is not restored at shutdown** (decision): reboot and poweroff reset the console font; schema-init never hands a running console to another init. The font *is* rolled back on a failed load (§2).

Palette (ANSI slot → RGB), from the approved mockup:

| slot | use | RGB |
|---|---|---|
| 0 black | background | 07 06 0b |
| 1 red | CUT, errors | ff 4d 5e |
| 2 green | ONLINE | 3d dc 84 |
| 3 yellow | RETRY / friction | ff d2 3f |
| 4 blue | STOPPED / dormant | 3d 7b ff |
| 5 violet | PERFECT, banner, track | 8a 6c ff |
| 6 cyan | `//`, finish text | 3f e0 ff |
| 7 white | text | c8 c8 d0 |
| 8 bright black | dim (times, separators) | 5c 58 70 |
| 9–15 bright | bright variants | ff 80 90, 7d ff b0, ff e6 80, 6f a0 ff, a9 9c ff, 90 f0 ff, ec ec f2 |

Retuned 2026-11-20 (#555): slots 4, 5, 12, 13 moved from the original `4da3ff`/`c46cff`/`80c0ff`/`e29bff` — the purples read pink.

SPAWN's dim green (1f 7a 4a) has no slot of its own: SPAWN uses green foreground with the dim attribute, or the builder may repurpose a slot if dim renders poorly on fbcon.

### 3. The schema font

`fonts/schema-16x32.bdf` (source, committed) → `fonts/schema-16x32.psfu` (built) → embedded as a C array (`schema_font.c`, generated at build time, no runtime file read). Built from Spleen 16×32, cut down to 256 glyphs: ASCII, Latin-1 printable, light box drawing, and the rail glyphs (BSD-2-Clause; keep its copyright notice in `fonts/LICENSE.spleen` and the package docs).

Glyphs the design needs that Spleen lacks or approximates, drawn in our font with correct Unicode mappings: `━ ┃ ┣ ┗ ╍` (heavy track), `◆ ◇ ▲ ■ □ ►`, `▓ ▒ ░` (banner blocks, Spleen's are kept if they map right). Every glyph the renderer emits must be in the font; a build-time test renders the full set of emitted strings through the font's Unicode table and fails on any unmapped code point.

When the font was **not** loaded (skipped or failed), the renderer uses a **strictly ASCII** alphabet for everything it emits — track and glyphs (`|`, `+-`, `*`, `o`, `^`, `#`, `>`), separators (`-` for `·`), truncation (`~` for `…`), banner (`== S C H E M A//I N I T ==`). A test runs every renderer path in fallback mode and asserts every byte is ASCII. Which alphabet is used is decided once, from whether our font loaded.

**Service names** come from .svc files and may contain anything. Padding and truncation count terminal columns, not bytes: in font mode a code point with a glyph in our font is one column; any code point without one (and every non-ASCII byte in fallback mode) is shown as `?`. Names cut at 17 columns with the mode's truncation mark. Tests: names shorter, equal, longer than 17 columns, and multibyte UTF-8.

### 4. What the styled rail shows

**Banner** (once per boot, before the first service line):

    ▓▒░ S C H E M A//I N I T ░▒▓  v0.4.29 · blakbox · 67 services
    ┃

Version, hostname, service count. Colours: blocks/`S C H E M A` magenta, `//` cyan, `I N I T` bright green, the rest dim.

**Service lines** during boot:

    ┣━◆ mount-fstab        ONLINE    +0.21s
    ┣━▲ bluetooth          RETRY 2   exit 1 · backoff 2s
    ┣━■ mcelog             CUT       exit 1 · not critical

Track + glyph in the state's colour, name in bright white padded to 17, chip = reverse video in the state's colour (black text on the colour), then either time since PID 1 started (dim) or, when something went wrong, the reason in the chip's colour. Every boot line uses `┣━` (decision: PID 1 can't know a line is the last one, and no cursor rewriting — child/kernel output can land in between); the finish line's `━━►` closes the track.

**Finish line**, once, when the rail settles (defined below):

    ━━► [65 ONLINE] [1 FRICTION] [1 CUT]  rail settled in 2.71s

Each loaded service lands in **exactly one** bucket by its state at that moment, and the buckets sum to `svc_count` (a unit test asserts the total for every state):

| bucket | chip | services |
|---|---|---|
| ONLINE | green | FUNDAMENTAL, SETTLED, FULL_TRUST, PERFECT; oneshots that finished successfully |
| WAITING | dim | not started this boot by design: condition false, socket-held, timer/path before first fire, stopped/standby, EXCISED with no failure (`failed=0`) |
| FRICTION | yellow | FRICTION, RECOVERY, DORMANT (degraded, still trying) |
| CUT | red | EXCISED or DORMANT-given-up with `failed=1` (the engine's `svc_failed()`) |
| STARTING | yellow | still starting (only possible on the 120 s timeout line) |

Zero buckets are omitted. The engine's `svc_failed()` is the source of truth for CUT — the same predicate verify's settle wait uses.

**After settle** (runtime): same line shape with a dashed track `╍━` and wall-clock `HH:MM:SS` instead of `+t`.

**Shutdown**: `▓▒░ S C H E M A//H A L T ░▒▓`, then one line per service that was running when shutdown began, and `━━► all down in 1.8s · powering off` / `· rebooting`. Decision: these lines are **rendered from service state**, not new `service_log` events, so `rail.log` and the shutdown log are unchanged. The set is a **snapshot taken once** at the start of the sweep (name + pid of every service with a live process). Each then gets its real outcome:
- gone after the polite stop (SIGTERM / stop command) → `STOPPED` (blue);
- had to be SIGKILLed (or cgroup-killed) → `KILLED` (red), not STOPPED;
- already gone before its stop (exited between snapshot and stop) → `EXITED` (dim);
- still there when the sweep gives up → `STUCK` (red). No separate UNMOUNTED chip: the existing `[schema-init] shutdown: …` messages (filesystems read-only, sync done, …) render as dim `┣━` lines on the track; their text is unchanged in the shutdown log.

### 5. Event → chip mapping

The rail emits ~60 event names (`service_log(svc, event)`). Styled mode maps each to one of: **shown** with a chip, or **quiet** (rail.log only).

| chip | colour | events (and state) |
|---|---|---|
| ONLINE | green | first entry into FUNDAMENTAL/SETTLED/FULL_TRUST (`promote`, `notify-ready`, `bus-name-ready`) |
| PERFECT | magenta | entry into PERFECT |
| DONE | green | `oneshot-done` |
| SPAWN | dim green | `spawn` — **only** if the service is *still starting* 2 s later (so a slow/hung start is visible without doubling every line); cancelled if it became ready, failed, or was cut in the meantime — never a stale SPAWN after the outcome |
| WAITING | dim green | `socket-wait`, `wait-socket` (held on its socket) |
| RETRY n | yellow | `retry`, `retry-deep`, `friction`, `restart-hold`, `died` **with a restart coming** |
| DORMANT | blue | `dormant` |
| CUT | red | `start-timeout`, `breaker-tripped`, `spawn-fail`, entry into EXCISED with a failure, `watchdog-kill`, `fuse-tripped`, `died` **with no restart coming** |
| STOPPED | blue | an asked-for stop that ended cleanly (`schema-ctl stop`, shutdown SIGTERM) |
| EXITED | dim | a service that ended on its own with success and no restart (not a oneshot) |
| TIMER | cyan | `timer-fire` (runtime only) |

Everything else is quiet on the VT (still in rail.log). The builder fills in the exact table from the event list in the code; the rule is: **anything that means "broken or degraded" is shown, in red or yellow; routine plumbing is quiet.** The mapping is a function of event **and** service state (`died` is RETRY or CUT depending on whether a restart is coming). Tests:
- every event name the source emits has an explicit entry (a new event can't silently land as quiet);
- every entry marked quiet carries a one-line justification in the table, and the review of this PR checks each failure-sounding one (`*-fail`, `*-timeout`, `*-abort`, `*-exhausted`, `died`, `fallback-down`, `readiness-lost`) by hand;
- state-dependent cases (`died` ± restart, EXCISED ± failed) are tested both ways.

### 6. "Rail settled"

PID 1 has no boot-complete notion today. **Settle detection is armed only after** the engine has loaded every service, resolved dependencies, and dispatched its initial start pass (the first engine tick that spawned or queued the eligible services); before that the predicate is never evaluated, so an empty queue at load time can't print an instant finish line. Then: the first moment after start when **no service is actively starting** — none has been spawned and not yet reached a ready state — and none is queued waiting on a dependency that is itself starting; checked on each state change. Services that never start this boot (condition false, disabled, waiting on a dep that will never come, socket-held, timers/paths before their first fire) do not hold it open. "Services" in the banner count = what the engine loaded (`svc_count`). The builder pins the exact predicate against the engine's states, with a unit test for each of those cases. Print the finish line then, with the time of the last transition. If it never settles within 120 s, print the finish line anyway with `rail still starting after 120s` in yellow and the counts, so the screen never just stops. Once printed, later lines use the runtime shape; the finish line is printed **at most once per boot** — a hung service that becomes ready after the 120 s line gets a runtime line, not a second finish line. A re-exec does not reprint banner or finish line.

### 7. Packaging (approach C's extra)

- rpm: `/usr/lib/kbd/consolefonts/schema-16x32.psfu.gz`; deb: `/usr/share/consolefonts/schema-16x32.psf.gz`. Owned by the main package.
- Docs: README section with a screenshot of the rail; how to `setfont schema-16x32`; how to add `FONT=schema-16x32` to vconsole.conf + rebuild the initramfs for the font from the first line; `schema.rail=plain` to turn it off.

## Testing

- **Unchanged paths**: vmtest (serial) passes untouched; `rail.log` lines byte-identical to today for a fixed event sequence (golden test).
- **Renderer unit tests** (pure function: event + service state + phase → styled line), golden strings for every chip, banner, finish, runtime, shutdown; ASCII-fallback set; width padding with long names (cut with `…` at 17).
- **Mapping test**: every event name in the source has an explicit mapping.
- **Font test**: the PSF parses, is 16×32, exactly 256 glyphs, and maps every code point the renderer can emit.
- **VT test**: in a QEMU run with `console=tty0` (vmtest variant with a VGA screen + screendump), PID 1 loads font and palette, banner + finish line appear; one screenshot checked by eye by Jonathan before merge.
- **Failure tests** (testhooks per ioctl): fail `KD_FONT_OP_SET`, `PIO_UNIMAPCLR`, `PIO_UNIMAP`, `PIO_CMAP` one at a time → console state rolled back (or the restore-FAILED line), plain lines, boot unaffected; palette apply failing after a successful save → no `console-orig` written, nothing restored at shutdown.
- **Re-exec palette test**: stock palette → schema palette → re-exec → shutdown restore → the **stock** palette (not the schema one) is back; no second banner/finish after a re-exec during boot.
- **Shutdown restore on a serial `/dev/console`** (`console=tty0 console=ttyS0`): shutdown output plain, palette still restored via `/dev/tty0`.
- **Settle tests**: empty queue before the first dispatch → no finish line; hung service → 120 s line, later readiness → runtime line, no second finish; counts sum to `svc_count` for every state.
- **Delayed SPAWN**: ready or failed before 2 s → no SPAWN line.
- **Shutdown outcomes**: STOPPED / KILLED / EXITED (exits between snapshot and stop) / STUCK, from a stubbed sweep.
- **User font policy**: vconsole.conf names another font → our font skipped, styling kept; `nofont` in `/etc/schema-init/rail` or `schema.rail_font=off` → skipped.
- **KDGETMODE** checked on the real fds PID 1 uses: stdout as set up at boot and the shutdown `/dev/console` fd, in the QEMU VT run.

## Left to the builder

- Exact padding, the reason strings per event, which ONLINE trigger fires first per readiness mode.
- Drawing the extra glyphs (pixel art at 16×32, matching Spleen's stroke weight); BDF→PSF tooling (a small script; `bdf2psf` from console-setup is fine as a build dependency or a tiny Python converter committed to the repo).
- How `shut_write` lines are styled, within the rule that their logged text is unchanged.
