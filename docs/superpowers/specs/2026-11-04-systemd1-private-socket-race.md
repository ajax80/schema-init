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

`_bind_private_socket(path)` binds the socket inside a 0700 throwaway dir under
`/run/systemd`, narrows it to 0600, then `os.rename()`s it onto the real path.
The rename is atomic and the listening inode is unaffected, so clients reach the
final path while `/run/systemd`'s own mode is never touched. Nothing a crash in
the window can skip leaves a shared dir corrupted or a 0777 socket at the real
path (an earlier dir-lock version did both — fixed after code review).

## Verified

`tests/test_systemd1_socket.py`: the socket ends 0600 at the real path, the
parent dir mode is untouched, no staging dir is left, a stale socket is replaced
atomically, and a failed bind leaves nothing behind — all compared against the
dir's actual prior mode, not an absolute. Live on Eli: after restart the socket
is `srw------- root:root`, `/run/systemd` stays 0755, no staging dir, the bridge
serves the system bus (nobody mutate denied).
