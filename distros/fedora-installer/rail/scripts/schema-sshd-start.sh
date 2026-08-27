#!/bin/bash
# schema-sshd-start.sh — bring up OpenSSH under schema-init as PID 1.
# There is no systemd sshd.service here; schema-init supervises sshd directly.
# Generate host keys on first boot if absent, then run sshd in the foreground
# (-D) so schema-init can supervise and restart it.
[ -f /etc/ssh/ssh_host_ed25519_key ] || ssh-keygen -A
mkdir -p /run/sshd
exec /usr/sbin/sshd -D
