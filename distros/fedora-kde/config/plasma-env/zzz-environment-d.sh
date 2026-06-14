#!/bin/sh
# schema-init has no `systemd --user`, so nothing applies environment.d(5) the way
# a normal user session would. Packages (flatpak, mesa, etc.) drop env config in
# these dirs expecting the session to pick it up; here nothing does. Replay them
# before the session bus/compositor start. Load order mirrors systemd: system dirs
# first, user last (user wins). Runs after the targeted env/*.sh hooks (zzz- prefix).
for dir in /usr/lib/environment.d /etc/environment.d "$HOME/.config/environment.d"; do
    [ -d "$dir" ] || continue
    for f in "$dir"/*.conf; do
        [ -f "$f" ] || continue
        while IFS= read -r line; do
            case "$line" in ''|\#*) continue ;; esac
            case "$line" in *=*) ;; *) continue ;; esac
            key=${line%%=*}
            val=${line#*=}
            # environment.d expands ${VAR}/$VAR against the current env. These dirs
            # are trusted system/user config, so eval-expand the value side.
            val=$(eval "printf '%s' \"$val\"" 2>/dev/null)
            export "$key=$val"
        done < "$f"
    done
done
