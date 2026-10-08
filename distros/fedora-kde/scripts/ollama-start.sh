#!/bin/sh
exec setpriv --reuid=ollama --regid="$(id -g ollama)" --init-groups -- env HOME="$(getent passwd ollama | cut -d: -f6)" USER=ollama LOGNAME=ollama /usr/local/bin/ollama serve
