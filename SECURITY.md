# Security policy

schema-init runs as PID 1 and replaces logind, udev and the D-Bus broker, so a bug here can hand out root, devices or the power button. Reports are welcome and taken seriously.

## Supported versions

Only the latest release gets fixes. Fixes ship as a new release (and to the COPR stable channel), not as backports.

## Reporting a vulnerability

Report it privately through GitHub: the **Security** tab of this repository → **Report a vulnerability**. Please don't open a public issue or pull request for it.

Include what you can of:

- the component (PID 1, schema-ctl, schema-logind, schema-udev, schema-dbus, schema-user, an installer script, …) and the version (`schema-init --version`);
- what an attacker needs to start with (local user, lingering user, a crafted unit file, a crafted boot entry or root filesystem, network access);
- what they get, and steps or a proof of concept that shows it.

## What to expect

This is a small project maintained in spare time, so these are targets, not guarantees:

- an acknowledgement within 7 days;
- an assessment (accepted, needs more information, or not a vulnerability, with the reasoning) within 14 days;
- for an accepted report, a fix released as soon as it is ready, usually within 30 days, and a published GitHub security advisory crediting you unless you ask not to be named.

If a fix needs longer, you'll hear why. Please give us until the fix is released, or 90 days from your report, whichever comes first, before disclosing publicly.

## Scope

In scope: everything in this repository that ships in a release or package, including the systemd compatibility shims and the installer.

Out of scope: problems that need root to begin with, unless they cross a boundary the project claims to hold (for example, `schema-ctl verify` running code from a boot entry it was only asked to inspect); bugs in the upstream software schema-init launches; and the `docs/` examples.

The project's audits so far have been AI-assisted reviews, not an independent professional audit.
