#!/bin/sh
exec setpriv --reuid=ollama --regid=ollama --init-groups --reset-env -- /usr/local/bin/ollama serve
