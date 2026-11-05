# schema-systemd1: close the private-socket creation race

## Problem

`main()` created `/run/systemd/private` then narrowed it: `PrivateSocketServer(path)`
binds the listening socket, and `os.chmod(path, 0o600)` runs after. dbus binds the
socket at **mode 0777 regardless of the process umask** (measured), so between the
bind and the chmod any local user can `connect()`. A private-socket connection has
`sender=None`, which `authorize()` trusts as root — so a connection won in that
window is handled as root for its lifetime (StartUnit/StopUnit/SetEnvironment on
anything). The window is small but recurs on every bridge start and restart
(a package upgrade restarts it).

Greg's audit suggested a umask fix; that does not help here, because the mode is
0777 irrespective of umask.

## Fix

`_bind_private_socket(path)` locks the parent dir (`/run/systemd`) to 0700 across
the bind, so only root — which bypasses directory permissions — can reach the
socket until it is chmod'd to 0600, then restores the dir mode in a `finally`
(a failed bind must not leave `/run/systemd` at 0700). Root consumers of
`/run/systemd/*` (schema-logind's sessions) are unaffected during the window;
non-root is blocked, which is the point.

## Verified

`tests/test_systemd1_socket.py`: socket ends 0600, parent dir mode preserved,
and preserved even when the bind raises. Live on Eli: after restart the socket
is `srw------- root:root`, `/run/systemd` stays 0755, the bridge serves the
system bus (nobody reads, nobody mutate denied).
