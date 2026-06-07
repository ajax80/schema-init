#!/bin/sh
(
    exec >> /var/log/schema-init/nordvpnd-wrapper.log 2>&1
    echo "Wrapper subshell started at $(date)"
    for i in $(seq 1 40); do
        if [ -S /run/nordvpn/nordvpnd.sock ]; then
            echo "Found socket, changing ownership..."
            chown root:nordvpn /run/nordvpn/nordvpnd.sock
            chmod 660 /run/nordvpn/nordvpnd.sock
            ls -la /run/nordvpn/nordvpnd.sock
            # Wait a bit and check again to ensure nordvpnd didn't overwrite it
            sleep 1
            chown root:nordvpn /run/nordvpn/nordvpnd.sock
            chmod 660 /run/nordvpn/nordvpnd.sock
            break
        fi
        sleep 0.5
    done
    echo "Wrapper subshell finished at $(date)"
) &
exec /usr/sbin/nordvpnd
