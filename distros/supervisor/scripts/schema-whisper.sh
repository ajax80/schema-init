#!/bin/sh
MODEL=/usr/local/share/whisper/ggml-small.en.bin
PORT=8765

i=0
while [ $i -lt 15 ] && [ ! -S /run/user/1000/pipewire-0 ]; do
    sleep 1
    i=$((i+1))
done

exec /usr/local/bin/whisper-server \
    --model "$MODEL" \
    --host 127.0.0.1 \
    --port "$PORT" \
    --language en \
    --no-timestamps
