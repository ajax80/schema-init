#!/bin/sh
WHISPER_URL=http://127.0.0.1:8765
LLAMA_URL=http://192.168.8.102:11434
LOG=/var/log/schema-init/supervisor-agent.log

mkdir -p /var/log/schema-init

i=0
while [ $i -lt 20 ] && ! curl -sf "$WHISPER_URL/health" >/dev/null 2>&1; do
    sleep 1
    i=$((i+1))
done

exec /usr/local/bin/supervisor-agent \
    --whisper "$WHISPER_URL" \
    --llama "$LLAMA_URL" \
    --nodes /etc/daedalus/nodes.conf \
    >>"$LOG" 2>&1
