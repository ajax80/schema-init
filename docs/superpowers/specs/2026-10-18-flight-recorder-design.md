# Flight Recorder — "what just happened" on the board

**Status:** Draft 2026-10-18, from a brainstorm with Jonathan; builder check + Greg review folded in. Origin: Greg's idea #4
(see memory `project_greg_bold_ideas_20261018`), reshaped by a real incident.
**Rule for this whole feature (Jonathan, 10-18):** try it, keep it only if it adds
without taking anything away. Every part is measured; anything that costs boot
time, PID 1 size, or idle CPU gets cut.

## Why

On 2026-09-22 Jonathan was scrolling Lollypop when the desktop vanished and he was
dropped back to the rail. The cause: Dolphin's video preview handed an `.mkv` to
`cosmic-player --thumbnail`, which spun forever; eight copies pegged ~1.35 cores
each for 20+ minutes and the desktop session died under it. He opened the board
and saw nothing useful — every service was green. PID 1's rail log had nothing
either: sddm never restarted, and the desktop session is not a PID 1 service.
By the time we looked into it weeks later, the logs from that week had rotated out.

Jonathan's words on what would have helped: one line like

```
▶ 21:42 desktop session ended — CPU pinned 20 min by cosmic-player ×8
```

"With that info I believe I may have been able to rectify the problem myself."
And: "the more info the better unless it comes at a cost."

**Key consequence:** a ring of PID 1's own service events alone (Greg's original
shape) would have been empty for this incident. The recorder must take events
from the places that actually saw it.

## Goals

1. When something takes the desktop down or chokes the box, the board shows an
   arrow line naming what happened and who did it.
2. `schema-ctl trace` shows the full recent story (the lead-up, with detail).
3. Recurrence is visible ("3rd time this week, same culprit").
4. PID 1's rail log lines carry a date.

## Non-goals

- Fix hints ("pkill cosmic-player; check /usr/share/thumbnailers") — later add-on,
  highest effort and easiest to get wrong. Not in this build.
- Greg's pstore flush. pstore only helps when PID 1 itself panics; Jonathan's
  incident was ordinary userspace. Dropped for now (my call — revisit if a PID 1
  panic ever leaves us blind).
- Replacing journal-sink / rail.log / coredump storage. The recorder is an index of
  notable events, not a log store.
- A graphical surface. The board (text, `schema-board.c`) is the surface.

## Anti-goals (failure even if it works)

- Measurable boot-time regression, PID 1 RSS growth beyond noise, or any
  periodic wakeup while the box is calm.
- A `schema_shm_t` layout bump (magic `0x53434d03`) — that forces board/desktop
  size-check churn across the fleet. The recorder uses its own segment.
- PID 1 blocking on, or depending on, the recorder. If the recorder is dead, PID 1
  behaves exactly as today.
- Noise: an arrow for things that aren't failures (a normal logout, a service
  stopped on purpose with `schema-ctl stop`, a timer job's normal exit).

## What gets recorded

Each event: realtime timestamp, boot id, source, kind, severity, subject (service /
process / session), and a short detail string. Sources and kinds:

| Source | Events | How it reaches the recorder |
|---|---|---|
| PID 1 (`service.c`/`init.c`) | service died unexpectedly (exit code / signal), restart, gave up (dormant/excised), limp card played, survival executive freeze/throttle/reclaim enter/leave | datagram from the existing log/state-change points; non-blocking, fire-and-forget |
| desktop session (`schema-plasma-autologin.sh` under `sddm.svc`) | Plasma exited (rc), session relaunched; PID 1's existing `sddm` died/restart event covers the non-zero path | datagram from the script next to its `plasma_exited rc=` line (e.g. a tiny `schema-recorder-send` helper) |
| schema-coredump (`schema-coredump.c`) | process crashed: comm, exe, signal, core path | datagram next to the existing `syslog(LOG_ERR, ... killed by SIG ...)` |
| earlyoom | process killed for memory | earlyoom's `-N /path/script` hook → small helper sends a datagram. There is no shipped `earlyoom.svc`; add `-N` in each host's own `earlyoom.svc` args (today only `distros/fedora-kde/hosts/blakbox`). If a host already passes `-N`, leave it — earlyoom takes one script. The recorder works without this source |
| recorder's own PSI triggers | CPU or memory pressure episode start/end, with the hogs | kernel PSI triggers on `/proc/pressure/cpu` and `/proc/pressure/memory`, `poll()` POLLPRI |

**Hog capture (when a pressure trigger fires):** never block the event loop. Take
snapshot 1 of `/proc/*/stat`, arm a 1 s `timerfd`, return to `poll()`; when it fires
take snapshot 2 and compute (Greg). Then take the top consumers, group identical comms (`cosmic-player ×8`), and for
the top group record: parent chain (`cosmic-player ← kioworker ← dolphin`),
truncated command line (`--thumbnail video_….mkv`), and box state at that moment:
CPU/memory pressure, swap and zram use, CPU temperature. Re-sample while the
episode lasts to keep the duration and culprit current, backing off to every
15–30 s after the first sample so a 20-minute episode is ~60 sweeps, not 1,200 (Greg); the episode ends after
pressure stays below the trigger for a quiet period (builder's choice, ~30 s).

**Desktop session source (settled after builder check):** on the fedora-kde and
fedora-installer profiles the session is run by `schema-plasma-autologin.sh`
(`sddm.svc`, critical). It registers the logind session with itself as leader, waits
on Plasma, relaunches after a 0 exit, and exits (so PID 1 logs `sddm` died and
restarts it) after a non-zero one. schema-logind therefore never sees an abnormal
leader exit and is **not** a source. The script reports `plasma_exited rc=` and the
relaunch; PID 1 reports the sddm death. Other desktop launchers (cinnamon etc.) are
out of scope for now.

**Correlation (session ended → why):** a Plasma exit within a short window of a
crash or kill of a session process (kwin, plasmashell, …), or inside a pressure
episode, is reported as one arrow line naming the likely cause. A clean logout and
a crash that exits 0 look the same to the script, so correlation is the only
judge: a Plasma exit 0 with no crash, kill, or pressure nearby is recorded but gets
no arrow; any non-zero exit gets an arrow.

PSI trigger write gotcha (already learned, see
`docs/superpowers/specs/2026-09-30-psi-triggers-design.md`): write the trigger
string **including the trailing NUL**, or the window gets chopped and the write
fails `EINVAL`. The recorder's triggers are its own fds, independent of PID 1's
survival-executive trigger.

**CPU noise guard (my call):** a compile can hold CPU `some` pressure above the
trigger for minutes. A CPU-only episode earns an arrow only if it lasts ≥ 2 minutes
or overlaps a crash, kill, service death or session end; shorter ones go to the
trace only. Memory episodes follow the same rule.

## Architecture (my call)

A new small daemon, **`schema-recorder`**, run as a normal `.svc` (non-critical,
needs root for `/proc` and PSI):

- Listens on a unix datagram socket under `/run/schema-init/` (root-only). All
  sources send one-line events to it. Senders never block and never retry; a
  missing socket means the event is dropped.
- Owns its own shm segment with its own magic and seq (the `schema_shm.h`
  pattern). The **current arrow** (state: none/live/held/recent, timestamp,
  recurrence, pre-rendered line) sits in a small header **ahead of** the event
  ring, so the board snapshots only the header, not the whole ring (Greg).
  `schema-ctl trace` copies the ring on demand. Both read without root.
- Appends every event to its own daily file,
  `/var/lib/schema-init/events-YYYY-MM-DD.log` (uncompressed), and deletes files
  older than 7 days itself — no logrotate, no zlib (Greg). On start it reloads the last ~36 h into the ring
  so the arrow survives a reboot, and scans the last 7 days for per-culprit
  counts, so "3rd time this week" spans boots.
- **Culprit identity** (my call): process `comm` (`cosmic-player`) for hogs,
  crashes and kills; service name for PID 1 events. Same comm from a different
  parent still counts as the same culprit.
- Zero wakeups while calm: it sleeps in `poll(..., -1)` on the socket, the two PSI
  fds and one `timerfd`. Timer state machine (Greg): calm = disarmed; trigger fired
  = 1 s (snapshot 2); active episode = 15–30 s re-samples / quiet-period tracking;
  quiet period expired = record the resolution, disarm, back to `poll(-1)`.
- Creates its shm with explicit `fchmod(fd, 0644)` (root daemons run with umask
  0077; without it, non-root board/trace readers get `EACCES`).

Why a separate daemon rather than inside PID 1: PID 1 size/RSS is a kill
criterion, and the `/proc` scanning and correlation logic doesn't belong in PID 1.
PID 1's only change is sending datagrams at points where it already logs.

Language is left to the builder, but it must meet the zero-idle-wakeup and RSS
goals; C next to `schema-coredump.c` / `schema-board.c` is the natural fit.

## The board arrow (Jonathan decided)

- **Bright red arrow** while the problem is live (e.g. the hog is still pinning
  the CPU).
- **Held bright for 12 hours** after it resolves — so it can't be missed if he was
  away when it happened. (Jonathan: "around 12 hours".)
- **Then grey** "this happened recently" for another 24 hours (my call).
- **Then off the board**, but still in `schema-ctl trace` and in the recurrence
  count. The same culprit returning goes straight back to red with "2nd time",
  "3rd time this week".
- One line, the most recent/most severe failure. Format like the example above.
  Precedence: live problem > recent session crash > held critical-service death >
  held minor failure; newest wins within a tier.
- The recorder stores `resolved_time`; **the board computes bright vs grey vs gone
  from `now - resolved_time`** on its own redraw, so the recorder never needs a
  12 h / 36 h wakeup (Greg). The board needs a slow redraw tick (e.g. once a
  minute) for the fade, in addition to redraws on seq change.
- Placed directly above the service table header, truncated to the terminal
  width (≤ cols − 2) so it never wraps; parent chains and command lines live in
  `schema-ctl trace` (Greg).

"Resolved" means: the pressure episode ended, the crashed/killed thing is gone,
or the service is back up — whichever applies to that event.

## `schema-ctl trace`

Prints the ring newest-last with full detail (parent chains, command lines, box
state). Reads the recorder's shm; no root, no PID 1 round-trip. Exact flags are
the builder's call (at least: all recent, and since-a-time).

## Autologin exit-code fix (prerequisite)

`schema-plasma-autologin.sh` ends its loop with `[ $RC -ne 0 ] && break` and
nothing after `done`, so the script exits 0 after a Plasma crash (Greg). PID 1
still restarts sddm (it is not a oneshot, so exit 0 is an unexpected death), but
the rail log and the recorder see `0`. Change to `exit "$RC"`. Repo copy:
`distros/fedora-kde/scripts/schema-plasma-autologin.sh`. Ships as its own small PR.

## Dated rail log lines

`service_log()` (`service.c:1630`) writes `[HH:MM:SS]`. Add the date. Check every
parser of `rail.log` first (`schema-init.spec`, `tests/livetest/vmtest.sh`, and the
specs that reference it) and keep them working.

## Measurement and kill criteria

Prototype on a branch, then measure on blakbox and Eli (3 boots before, 3 after):

- boot time (`schema-ctl analyze` / boot-timing log) — no regression beyond noise
- PID 1 RSS — unchanged beyond noise
- recorder RSS — small (target well under 2 MB)
- idle wakeups — recorder `voluntary_ctxt_switches` delta over 10 calm minutes ≈ 0
- LOC added, per part

Report the numbers to Jonathan. Any part that costs gets cut.

**Proof it works:** replay the 09-22 incident in a contained cgroup — a CPU hog
spawned under a fake parent chain, then make Plasma exit (not the autologin
script) — on a VM or test session, not Jonathan's live desktop — and see the
arrow line, the 12 h hold behaviour (with an injectable clock for tests), and
`schema-ctl trace` detail.

## Where it runs (Jonathan decided)

**Opt-in per host**, like `schema-board.svc.example`: the RPM ships
`schema-recorder.svc.example`; it is enabled on blakbox, Eli and Optiplex. No
fallbacks needed for profiles without PSI (Pi Zero W, artix, void, devuan) — if
PSI is missing the recorder still records pushed events and logs one line saying
pressure capture is off.

## Starting defaults (Greg; builder may tune after measuring)

- Socket `/run/schema-init/recorder.sock`, `AF_UNIX`/`SOCK_DGRAM`, mode 0600 (all
  senders run as root).
- Wire format: one line of `key=value` fields (`ts= src= kind= comm= pid= rc= msg=`).
- PSI: CPU `some 250000 1000000`, memory `some 200000 1000000`, trailing NUL.
- Episode quiet period 30 s; correlation window 20 s before a session exit.

## Left to the builder

- Ring entry size and ring length.
- How many hog groups to keep, and sampling details.
- `schema-ctl trace` flags and output layout.
- Tuning the starting defaults above after measuring.
