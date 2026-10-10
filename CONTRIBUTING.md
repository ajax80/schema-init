# Contributing

Thanks for looking. schema-init runs as PID 1, so the bar is simple: a change has to be proven on a real boot, not just compiled.

## Build and test

```sh
make          # static schema-init + tools
make test     # the unit suite: no root, no VM
```

`make test` must pass before a PR is opened. C code builds with `-std=c99 -Wall -Wextra`; don't add warnings.

## Before touching PID 1 on hardware

Boot the change in a VM first: `scripts/vmtest-gui.sh` (see [Quickstart](README.md#quickstart)). Anything that changes boot, shutdown, udev or D-Bus behaviour should also be run on a real box, with a systemd boot entry kept as a fallback. Say in the PR what you ran it on.

## Pull requests

- One change per PR. The title is the commit subject: `area: what changed` (`fix(udev): …`, `feat(import): …`, `docs: …`).
- The description says what was wrong, what changed, how it was verified, and what was **not** verified. Unverified claims stay out.
- Open it as a draft until it has been reviewed; PRs are squash-merged.
- New `.svc` keys are documented in [docs/svc-reference.md](docs/svc-reference.md) in the same PR.

## Security

Report anything that lets an unprivileged user affect PID 1, the control socket, the notify socket or schema-dbus through a private GitHub security advisory (Security tab → Report a vulnerability), not a public issue.

## License

schema-init is AGPL-3.0-or-later. By contributing you agree your work is released under it.
