#!/bin/sh
# schema-init: no systemd user env-generator runs flatpak's XDG_DATA_DIRS hook,
# so the session D-Bus daemon never learns about the flatpak exports dir and
# D-Bus activation of flatpak apps fails:
#   "The name org.telegram.desktop was not provided by any .service files"
# Prepend the flatpak export shares BEFORE the session bus starts.
for d in "$HOME/.local/share/flatpak/exports/share" /var/lib/flatpak/exports/share; do
    case ":${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:" in
        *":$d:"*) ;;
        *) XDG_DATA_DIRS="$d:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" ;;
    esac
done
export XDG_DATA_DIRS
