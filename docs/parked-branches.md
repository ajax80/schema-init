# Parked branches

Branches that were **not merged** and are **not on master**, kept only so their
history isn't lost. Each represents a superseded approach — the *problem* it
solved is handled a different way on master; the branch's code is the *old* way.

Preserved two ways: the branch (`parked/<name>`) holds the tip; an annotated tag
(`parked/<name>-note`) carries the rationale (`git tag -n1 -l 'parked/*'`).

Do not merge these. If a parked approach is ever revived, branch off the tip and
open a fresh PR.

| branch | tip | superseded by |
|---|---|---|
| `parked/kde-mount-guard` | `ed258b9` | watchdog/shim plasmashell path |
| `parked/mount-guard-replace-race` | `8817021` | watchdog owns plasmashell respawn |

## kde-mount-guard — `ed258b9`

A `kde-mount-guard` service that healed the plasmashell boot race by watching a
mount. Master solves the same race through the watchdog/shim path instead
(`plasmashell-shim`, `plasma-session-start.sh`, `schema-autostart-runner.sh`), so
no mount-guard files exist on master. The branch's own final commit ("let the
watchdog own plasmashell respawn, not the mount guard") already pointed at the
solution master shipped.

## mount-guard-replace-race — `8817021`

Made `kde-mount-guard`'s `--replace` self-heal. Moot once the watchdog owns
plasmashell respawn — the `--replace` race it guarded against no longer exists.

---
*Pruned from 97 branches → master on 2026-09-15. Everything else was either an
ancestor of master or squash-merged (verified by defining-artifact presence in
master's tree). These two were the only superseded-not-merged branches.*
