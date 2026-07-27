# schema-init Limp-Mode — Design

**Date:** 2026-06-14
**Author:** Jonathan Ayers (design) + Claude (drafting)
**Branch:** `feat/limp-mode`

## Problem

When a desktop freezes (e.g. kwin/Wayland wedges), the user has no graceful recovery
surface. `ctrl-alt-F2` only opens another login on the same broken session; systemd's
`rescue`/`emergency` targets are all-or-nothing — they tear the session down and lose
work. There is no **limp-mode**: a way to see what's wrong, fix the broken part in place,
preserve work, and resume — the OS equivalent of limping home on a degraded leg, reaching
into a bag for a replacement card, swapping it into the lit slot, and running again.

## Goal

A recovery surface that lives **below** the compositor — owned by PID 1, on its own VT —
so it survives a frozen desktop. It shows every service's weight-state at a glance (the
OS's "LED color-board", mirroring the Ungulate Leg LED language), and offers the
mitigations that bring each back to PERFECT (weight/state 88).

## Settled Design Decisions

1. **Behavior — C with B as fallback.** schema-init auto-heals in the background
   (self-healer, C); the user only reaches into the bag (interactive cockpit, B) when the
   auto-healer hits a slot it can't card on its own.
2. **Cards — A with C as the empty-deck fallback.** Each service declares its own
   mitigation ladder in its `.svc` file (deterministic, author-defined). When a failure
   has no matching card, the engine generates one on the fly from the failure mode (the
   F8/F9/F6 flag vectors already describe *how* a service fell).
3. **Compositor — B, bridged.** The graphical session is one tile on the board, but the
   actual card-play is delegated to the existing `schema-plasma-watchdog`. PID 1 *shows*
   it; the in-session watchdog *fixes* it. Respects the PID1-vs-user-session boundary.
4. **Compositor card ladder is surgical → nuclear**, played cheapest-first:
   - **Poke** (work preserved): reset input pipeline, `SIGCONT` a stopped process, or
     kill the single hung client. Most freezes die here.
   - **Nudge**: have the watchdog revive plasmashell/the shell layer without bouncing the
     session.
   - **Nuclear** (full session restart = logout on Wayland, unsaved work lost): **never
     auto-played.** Stays the B-fallback — slot stays lit, hands the user the bag, fires
     only on explicit confirm with a "these N apps have unsaved work" warning.

## Architecture

```
PID 1 (init.c) ──writes──> POSIX shm "/schema-init" (schema_shm_t, already exists)
                                  │ read-only
                                  ▼
                         schema-board  (NEW, standalone process)
                         - mmap shm O_RDONLY, seqlock read on `seq`
                         - paints a VT (own tty), refreshes on seq change
                         - survives compositor freeze: independent process,
                           depends on nothing graphical
```

The board is a **separate read-only process**, not code wedged into PID 1's main loop.
This is the key to "unfreezable": it reads the shm snapshot and depends on nothing that
can wedge. The card *engine* (auto-healer + generative fallback) lives in init.c (it owns
the services); the board is its display, and later its B-cockpit front-end via the
existing `schema-ctl` control socket.

### Data contract (already exists — `schema_shm.h`)

`schema_shm_t { uint32 seq; int32 count; shm_svc_t svc[64]; int32 group_count;
shm_group_t groups[16]; uint8 system_state; }` where `shm_svc_t { char name[64];
uint8 state; uint8 weight; int32 child_pid; int32 restart_count; }`. PID 1 already
`shm_open`s this `O_CREAT|O_RDWR` and bumps `seq` per update (init.c:730/755). The board
is the first external reader; no producer changes needed for the read-only increment.

### State → color mapping (LED language)

| state | # | color | meaning |
|---|---|---|---|
| PERFECT | 88 | purple | go / heaven |
| FULL_TRUST / FUNDAMENTAL | 10 / 1 | green | stable |
| NEW_PROCESS | 8 | green (dim) | new / starting |
| SETTLED | 7 | green | settled |
| RECOVERY / FRICTION | 9 / 6 | yellow | failing / recovering |
| DORMANT | 75 | blue | attention (backoff anteroom) |
| EXCISED | 76 | red | broke (sulphur gate) |

## Build Increments

- **Increment 1 — TONIGHT — the read-only board (the spine).** `schema-board.c`:
  open shm read-only, seqlock-consistent snapshot, render the service table with
  state colors + weight + pid + restart_count + system_state + groups; refresh on `seq`
  change. Verify it renders the live shm under the vmtest harness. No input, no fixing yet.
- Increment 2 — **BLOCKED on a logind gap (2026-07-25).** `--tty <device>` is implemented and
  the board paints a dedicated console (verified with `setterm --dump`), but the console is
  **not reachable on a graphical system at all** — healthy or wedged.

  Measured twice on real hardware:
  - ✅ **Survival proven** (07-24) — `SIGSTOP` on `kwin_wayland`, 30 s, 47-service desktop:
    `seq` advanced 82491 → 82639 with the compositor in state `T`.
  - ❌ **Reachability disproven, healthy desktop** (07-25) — with `kwin_wayland` running
    normally, `ctrl-alt-F8` moved `/sys/class/tty/tty0/active` to `tty8` and the operator
    saw no change. `ctrl-alt-F2` — a plain autologin console with no connection to the
    board — moved it to `tty2` with the same result. The kernel switches; nothing repaints.

  **Root cause — `scripts/schema-logind.py`, not `schema-board.c`.** The session-device
  handoff is one-way. It implements `TakeControl` and `TakeDevice` (so a compositor can
  acquire KMS) but has **no** `ResumeDevice`, **no** `Seat.SwitchTo`, **no** session `VTNr`,
  and **no** watch on `/sys/class/tty/tty0/active`. So the compositor is never told it lost
  the console, never drops DRM master, and the scanout never changes. `loginctl show-session`
  returns empty for the same reason. This breaks the tty2–tty6 gettys identically.

  **Two earlier conclusions were wrong and are retracted here:**
  1. "A *stopped* compositor cannot release DRM master." A healthy one does not release it
     either — nothing asks it to. The `SIGSTOP` was incidental.
  2. "The board must take DRM master itself (`VT_ACTIVATE` + `DRM_IOCTL_SET_MASTER`)." That
     cannot work: `SET_MASTER` fails while another process holds master.

  **Remaining work — in `schema-logind.py`:**
  1. Record a `VTNr` on each session at creation (from the session's tty / `XDG_VTNR`).
  2. Watch the active VT — poll `/sys/class/tty/tty0/active`, or `VT_WAITACTIVE` on
     `/dev/tty0` in a thread.
  3. On a transition, emit `PauseDevice(major, minor, "gone")` for every device the
     outgoing session took and set its `Active` to false; emit `ResumeDevice(major, minor, fd)`
     with a fresh fd for the incoming session and set `Active` true.
  4. Implement `Seat.SwitchTo(uint32)` and `Session.Activate()` so a compositor can request
     a switch itself.

  ⚠️ **This is the display path on the only working desktop.** A wrong `PauseDevice` makes the
  compositor drop KMS with nothing to hand it back: black screen, blind reboot. Gate on
  `schema-vmtest` before any hardware run, and keep `sudo chvt 1` reachable over ssh
  throughout. `SIGSTOP`/`SIGCONT` remains the correct freeze harness — killing a Wayland
  compositor ends the session, which is the exact cost limp-mode exists to avoid.
- Increment 3 — B-cockpit: arrow to a lit slot, apply a card via the `schema-ctl` socket.
- Increment 4 — `.svc` card-ladder syntax + auto-healer (C) playing declared decks.
- Increment 5 — generative fallback card from F8/F9/F6 flags when the deck is empty.
- Increment 6 — compositor tile bridged to `schema-plasma-watchdog`, surgical→nuclear
  ladder, nuke-on-confirm-only with unsaved-work warning.

## Testing

- Increment 1 verified via the `~/schema-livetest` vmtest harness: boot schema-init in
  QEMU, confirm `schema-board` reads the shm and prints the live service table matching
  the boot-rail markers. (Harness gotcha: the running custom no-initramfs kernel has no
  `vmlinuz` in its modules dir — boot a stock kernel, e.g.
  `/lib/modules/7.0.12-cachyos1.fc44.x86_64/vmlinuz`.)

## Non-Goals

- Not a full TUI process manager (htop). It shows schema weight-states and mitigations,
  nothing more.
- Does not replace `schema-ctl` for scripted control.
- Does not attempt cross-session work preservation beyond the surgical-first ladder; the
  nuclear card explicitly warns and costs unsaved work.
